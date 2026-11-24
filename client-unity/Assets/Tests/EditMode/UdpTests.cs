using System.Collections.Generic;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Threading;
using NUnit.Framework;
using Pong.Protocol;
using Relay.Net;

namespace Relay.Tests
{
    /// <summary>
    /// protocol.md 7장 데이터그램 형식(순수 바이트)과, 이 프로세스 안의 가짜 서버로 확인하는 묶기 · 포기 · 버리기.
    /// 가짜 서버는 TcpListener와 같은 포트 번호의 UDP 소켓이라 Docker 서버 없이 돈다.
    /// </summary>
    public class UdpTests
    {
        private const uint Token = 0xA1B2C3D4;

        [Test]
        public void Bind_Bytes()
        {
            // u32 udp_token + 프레임(01 18 00 00)
            CollectionAssert.AreEqual(new byte[] { 0xA1, 0xB2, 0xC3, 0xD4, 0x01, 0x18, 0x00, 0x00 }, UdpDatagram.Bind(Token));
        }

        [Test]
        public void GameIn_Datagram_RoundTrip()
        {
            var h = new PongInputHistory();
            h.Add(1, PongIds.AxisUp);
            h.Add(2, PongIds.AxisDown);
            byte[] d = UdpDatagram.Build(Token, ClientMessages.GameIn(h.EncodeBundle()));
            byte[] expected =
            {
                0xA1, 0xB2, 0xC3, 0xD4, // token
                0x01, 0x40, 0x00, 0x0B, // GAME_IN len 11
                0x02, 0, 0, 0, 1, 1, 0, 0, 0, 2, 2,
            };
            CollectionAssert.AreEqual(expected, d);
            Assert.IsTrue(UdpDatagram.TryParseClient(d, d.Length, out uint token, out Frame f));
            Assert.AreEqual(Token, token);
            Assert.AreEqual(MsgType.GameIn, f.Type);
            Assert.AreEqual(2, PongInput.Parse(f.Payload).Count);
        }

        [Test]
        public void ServerDatagram_AcceptsBoundAndGameOut()
        {
            byte[] bound = FrameEncoder.Encode(MsgType.UdpBound, null);
            Assert.IsTrue(UdpDatagram.TryParseServer(bound, bound.Length, out Frame f));
            Assert.AreEqual(MsgType.UdpBound, f.Type);
            Assert.AreEqual(0, f.Payload.Length);

            var s = new PongState { Tick = 120, LastInputSeq = 41, BallX = 400, BallY = 300, Paddle0Y = 300, Paddle1Y = 250, Score0 = 1, Phase = 1 };
            byte[] state = FrameEncoder.Encode(MsgType.GameOut, s.ToGameOut());
            // 받기 버퍼가 데이터그램보다 커도 count만큼만 본다.
            var buf = new byte[UdpDatagram.MaxDatagram + 1];
            System.Buffer.BlockCopy(state, 0, buf, 0, state.Length);
            Assert.IsTrue(UdpDatagram.TryParseServer(buf, state.Length, out f));
            Assert.AreEqual(MsgType.GameOut, f.Type);
            Assert.AreEqual(120u, PongState.Parse(f.Payload).Tick);
        }

        [Test]
        public void ServerDatagram_DropsMalformed()
        {
            byte[] good = FrameEncoder.Encode(MsgType.GameOut, new PongState { Tick = 1 }.ToGameOut());
            var bad = new List<(string why, byte[] data)>
            {
                ("empty", new byte[0]),
                ("short header", new byte[] { 1, 0xC0, 0 }),
                ("version 2", With(good, 0, 2)),
                ("len larger than bytes", Trim(good, 1)),
                ("extra byte after frame", Concat(good, new byte[] { 0 })),
                ("len over 512", new byte[] { 1, 0xC0, 0x02, 0x01 }),
                ("UDP_BOUND with payload", FrameEncoder.Encode(MsgType.UdpBound, new byte[] { 0 })),
                ("empty GAME_OUT", FrameEncoder.Encode(MsgType.GameOut, null)),
                ("GAME_OUT over 500", FrameEncoder.Encode(MsgType.GameOut, new byte[501])),
                ("TCP-only WELCOME", FrameEncoder.Encode(MsgType.Welcome, new byte[9])),
                ("TCP-only ERROR", FrameEncoder.Encode(MsgType.Error, new byte[] { 0, 1 })),
                ("client type", FrameEncoder.Encode(MsgType.GameIn, new byte[5])),
            };
            foreach (var (why, data) in bad)
                Assert.IsFalse(UdpDatagram.TryParseServer(data, data.Length, out _), why);
            Assert.IsFalse(UdpDatagram.TryParseClient(new byte[] { 1, 2, 3 }, 3, out _, out _), "client: no token");
        }

        // ---- 가짜 서버 ----

        /// <summary>TCP로 WELCOME만 보내는 서버와, 같은 포트 번호의 UDP 소켓.</summary>
        private sealed class FakeServer : System.IDisposable
        {
            public readonly TcpListener Tcp;
            public readonly UdpClient Udp;
            public readonly int Port;
            private TcpClient _accepted;

            public FakeServer()
            {
                // 같은 번호의 TCP · UDP가 모두 비어 있는 포트를 찾는다(UDP만 다른 프로그램이 쓰고 있을 수 있다).
                for (int attempt = 0; ; attempt++)
                {
                    Tcp = new TcpListener(IPAddress.Loopback, 0);
                    Tcp.Start();
                    Port = ((IPEndPoint)Tcp.LocalEndpoint).Port;
                    try
                    {
                        Udp = new UdpClient(new IPEndPoint(IPAddress.Loopback, Port));
                        break;
                    }
                    catch (SocketException) when (attempt < 5)
                    {
                        Tcp.Stop();
                    }
                }
                Udp.Client.ReceiveTimeout = 3000;
            }

            public RelayConnection ConnectClient()
            {
                var conn = new RelayConnection();
                conn.Start("127.0.0.1", Port, useUdp: true);
                _accepted = Tcp.AcceptTcpClient();
                var w = new Welcome { ConnId = 7, UdpToken = Token };
                w.Games.Add(new GameInfo { GameId = 1, MinPlayers = 2, MaxPlayers = 2, TickHz = 30, Name = "Pong" });
                byte[] frame = FrameEncoder.Encode(MsgType.Welcome, w.ToPayload());
                _accepted.GetStream().Write(frame, 0, frame.Length);
                return conn;
            }

            public void Dispose()
            {
                _accepted?.Close();
                Tcp.Stop();
                Udp.Close();
            }
        }

        private static RelayEvent WaitEvent(RelayConnection c, RelayEventKind kind, int timeoutMs)
        {
            var sw = Stopwatch.StartNew();
            while (sw.ElapsedMilliseconds < timeoutMs)
            {
                if (c.TryReceive(out RelayEvent e))
                {
                    if (e.Kind == kind)
                        return e;
                    Assert.AreNotEqual(RelayEventKind.Closed, e.Kind, "closed: " + e.Reason);
                }
                else
                {
                    Thread.Sleep(2);
                }
            }
            Assert.Fail($"no {kind} in {timeoutMs} ms");
            return default;
        }

        [Test, Timeout(15000)]
        public void NoUdpBound_GivesUpAfterTwoSeconds_TcpStays()
        {
            using (var server = new FakeServer())
            using (RelayConnection conn = server.ConnectClient())
            {
                var sw = Stopwatch.StartNew();
                var binds = new List<long>();
                var from = new IPEndPoint(IPAddress.Any, 0);
                // 묶기 동안 오는 UDP_BIND를 모두 받는다. 답은 하지 않는다.
                while (sw.ElapsedMilliseconds < UdpDatagram.BindGiveUpMs + 600)
                {
                    if (server.Udp.Available == 0)
                    {
                        Thread.Sleep(2);
                        continue;
                    }
                    byte[] d = server.Udp.Receive(ref from);
                    Assert.IsTrue(UdpDatagram.TryParseClient(d, d.Length, out uint token, out Frame f));
                    Assert.AreEqual(Token, token);
                    Assert.AreEqual(MsgType.UdpBind, f.Type);
                    binds.Add(sw.ElapsedMilliseconds);
                }
                RelayEvent failed = WaitEvent(conn, RelayEventKind.UdpFailed, 1000);
                StringAssert.Contains("UDP_BOUND", failed.Reason);
                Assert.AreEqual(UdpState.Failed, conn.UdpState);
                // 0.2초마다 2초 동안: 0, 0.2, ..., 1.8초 → 10번. 스레드 시작 · 타이머 흔들림을 1번 봐 준다.
                Assert.That(binds.Count, Is.InRange(9, 11), "UDP_BIND count");
                Assert.IsTrue(conn.IsConnected, "TCP must stay after UDP gives up");
                Assert.IsFalse(conn.SendUdp(ClientMessages.Ping(1)), "no UDP send after giving up");
            }
        }

        [Test, Timeout(15000)]
        public void Bound_GameInOverUdp_MalformedDatagramDropped()
        {
            using (var server = new FakeServer())
            using (RelayConnection conn = server.ConnectClient())
            {
                var from = new IPEndPoint(IPAddress.Any, 0);
                byte[] first = server.Udp.Receive(ref from);
                Assert.IsTrue(UdpDatagram.TryParseClient(first, first.Length, out _, out Frame bind));
                Assert.AreEqual(MsgType.UdpBind, bind.Type);
                byte[] bound = FrameEncoder.Encode(MsgType.UdpBound, null);
                server.Udp.Send(bound, bound.Length, from);
                WaitEvent(conn, RelayEventKind.UdpBound, 1000);
                Assert.AreEqual(UdpState.Bound, conn.UdpState);

                // GAME_IN은 UDP로: 토큰 + 프레임. 남아 있던 UDP_BIND는 건너뛴다.
                byte[] gameIn = ClientMessages.GameIn(PongInput.Encode(5, PongIds.AxisUp));
                Assert.IsTrue(conn.SendUdp(gameIn));
                Frame got;
                do
                {
                    byte[] d = server.Udp.Receive(ref from);
                    Assert.IsTrue(UdpDatagram.TryParseClient(d, d.Length, out uint token, out got));
                    Assert.AreEqual(Token, token);
                } while (got.Type == MsgType.UdpBind);
                Assert.AreEqual(MsgType.GameIn, got.Type);
                CollectionAssert.AreEqual(PongInput.Encode(5, PongIds.AxisUp), got.Payload);

                // 깨진 데이터그램은 버리고(연결 유지), 뒤의 STATE는 UDP 표시와 함께 같은 큐로 온다.
                server.Udp.Send(new byte[] { 9, 9, 9 }, 3, from);
                byte[] state = FrameEncoder.Encode(MsgType.GameOut, new PongState { Tick = 3 }.ToGameOut());
                server.Udp.Send(state, state.Length, from);
                RelayEvent e = WaitEvent(conn, RelayEventKind.Frame, 2000);
                Assert.IsTrue(e.ViaUdp);
                Assert.AreEqual(3u, PongState.Parse(e.Frame.Payload).Tick);
                Assert.AreEqual(1, conn.UdpDropped);
                Assert.IsTrue(conn.IsConnected);

                // 연결을 닫으면 UDP도 닫혀 더는 보낼 수 없다.
                conn.Close();
                Assert.IsFalse(conn.SendUdp(gameIn));
            }
        }

        private static byte[] With(byte[] src, int index, byte value)
        {
            var b = (byte[])src.Clone();
            b[index] = value;
            return b;
        }

        private static byte[] Trim(byte[] src, int n)
        {
            var b = new byte[src.Length - n];
            System.Buffer.BlockCopy(src, 0, b, 0, b.Length);
            return b;
        }

        private static byte[] Concat(byte[] a, byte[] b)
        {
            var r = new byte[a.Length + b.Length];
            System.Buffer.BlockCopy(a, 0, r, 0, a.Length);
            System.Buffer.BlockCopy(b, 0, r, a.Length, b.Length);
            return r;
        }
    }
}
