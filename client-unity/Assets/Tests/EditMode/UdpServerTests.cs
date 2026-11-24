using System.Collections.Generic;
using NUnit.Framework;
using Pong.Protocol;
using Relay.Net;

namespace Relay.Tests
{
    /// <summary>
    /// 실제 서버(Docker, -p 7777:7777 -p 7777:7777/udp)와 protocol.md 7장 UDP를 확인한다.
    /// UDP로 묶은 연결은 STATE를 UDP로 받고(GOAL은 TCP), GAME_IN을 UDP 묶음으로 보내 패들을 움직인다.
    /// </summary>
    [Category("Server")]
    public class UdpServerTests
    {
        private readonly List<ServerPeer> _peers = new List<ServerPeer>();

        private ServerPeer Connect(string name, bool udp)
        {
            ServerPeer p = ServerPeer.Connect(name, udp);
            _peers.Add(p);
            return p;
        }

        [TearDown]
        public void TearDown()
        {
            foreach (var p in _peers)
                p.Conn.Close();
            _peers.Clear();
        }

        // 서브 대기 3초 동안 공이 멈춰 있어 패들 이동만 볼 수 있고, 첫 득점으로 끝난다.
        private static readonly PongSettings Rules = new PongSettings { win_score = 1, serve_wait_ticks = 90 };

        private static void StartMatch(ServerPeer a, ServerPeer b)
        {
            a.Send(ClientMessages.CreateRoom(PongIds.GameId, Protocol.VisibilityPrivate, Rules.ToBytes()));
            var ja = RoomJoined.Parse(a.WaitFor(MsgType.RoomJoined).Payload);
            b.Send(ClientMessages.JoinByCode(ja.Code));
            Assert.AreEqual(1, RoomJoined.Parse(b.WaitFor(MsgType.RoomJoined).Payload).MySlot);
            a.WaitFor(MsgType.MatchStart);
            b.WaitFor(MsgType.MatchStart);
        }

        private static System.Action<ServerPeer> Via(bool udp, byte axis) =>
            udp ? (System.Action<ServerPeer>)(p => p.SendInputUdp(axis)) : p => p.SendInput(axis);

        /// <summary>
        /// A(slot 0)는 위, B(slot 1)는 아래로 1.5초(45진행, 끝까지 가는 데 21진행이면 충분) 보낸 뒤
        /// 두 패들이 벽에 붙었는지 본다. 이어서 정지 입력으로 경기를 끝까지 돌린다.
        /// </summary>
        private static void MovePaddlesThenFinish(ServerPeer a, bool aUdp, ServerPeer b, bool bUdp)
        {
            Assert.IsFalse(ServerPeer.Run(a, Via(aUdp, PongIds.AxisUp), b, Via(bUdp, PongIds.AxisDown), untilMs: 1500),
                "match must not end during the serve wait");
            // 마지막 입력이 반영된 STATE를 받을 시간을 조금 둔다(정지 입력을 보내며).
            ServerPeer.Run(a, Via(aUdp, PongIds.AxisNone), b, Via(bUdp, PongIds.AxisNone), untilMs: 300);
            int top = Rules.paddle_h / 2, bottom = Rules.field_h - Rules.paddle_h / 2;
            foreach (ServerPeer p in new[] { a, b })
            {
                Assert.IsTrue(p.HasState, $"{p.Name}: no STATE");
                Assert.AreEqual(top, p.Latest.Paddle0Y, $"{p.Name} sees paddle0 (A, up)");
                Assert.AreEqual(bottom, p.Latest.Paddle1Y, $"{p.Name} sees paddle1 (B, down)");
                Assert.AreEqual(PongIds.PhaseServe, p.Latest.Phase, $"{p.Name}: still serve wait");
                Assert.Greater(p.Latest.LastInputSeq, 40u, $"{p.Name}: server used this peer's inputs");
            }
            Assert.IsTrue(ServerPeer.Run(a, Via(aUdp, PongIds.AxisNone), b, Via(bUdp, PongIds.AxisNone)), "MATCH_END(0)");
            Assert.AreEqual(1, a.GoalsTcp, "A: GOAL is reliable → TCP");
            Assert.AreEqual(1, b.GoalsTcp, "B: GOAL is reliable → TCP");
            Assert.AreEqual(0, a.GoalsUdp + b.GoalsUdp, "GOAL never via UDP");
        }

        [Test, Timeout(60000)]
        public void BothUdp_StateViaUdp_GameInViaUdpMovesPaddles()
        {
            ServerPeer a = Connect("A", udp: true), b = Connect("B", udp: true);
            Assert.AreEqual(UdpState.Bound, a.Conn.UdpState);
            Assert.AreEqual(UdpState.Bound, b.Conn.UdpState);
            StartMatch(a, b);
            MovePaddlesThenFinish(a, true, b, true);

            foreach (ServerPeer p in new[] { a, b })
            {
                Assert.Greater(p.StatesUdp, 60, $"{p.Name}: STATE via UDP");
                Assert.AreEqual(0, p.StatesTcp, $"{p.Name}: bound → unreliable GAME_OUT only via UDP");
                Assert.AreEqual(0, p.Conn.UdpDropped, $"{p.Name}: server datagrams well-formed");
            }
        }

        [Test, Timeout(60000)]
        public void MixedRoom_UdpAndTcpPeers()
        {
            ServerPeer a = Connect("A", udp: true), b = Connect("B", udp: false);
            Assert.AreEqual(UdpState.Bound, a.Conn.UdpState);
            Assert.AreEqual(UdpState.Off, b.Conn.UdpState);
            StartMatch(a, b);
            MovePaddlesThenFinish(a, true, b, false);

            Assert.Greater(a.StatesUdp, 60, "A (UDP): STATE via UDP");
            Assert.AreEqual(0, a.StatesTcp, "A (UDP): no STATE via TCP");
            Assert.Greater(b.StatesTcp, 60, "B (TCP): STATE via TCP");
            Assert.AreEqual(0, b.StatesUdp, "B (TCP): nothing via UDP");
        }

        [Test, Timeout(30000)]
        public void MalformedUdpGameIn_DroppedSilently_ConnectionKept()
        {
            ServerPeer a = Connect("A", udp: true), b = Connect("B", udp: false);
            StartMatch(a, b);

            // 7장 묶기 4번: 형식이 틀린 UDP GAME_IN은 버리기만 한다(TCP였다면 BAD_GAME_INPUT으로 끊는다).
            var wrongN = new byte[11];
            wrongN[0] = 3; // 길이 11은 n = 2
            Assert.IsTrue(a.Conn.SendUdp(FrameEncoder.Encode(MsgType.GameIn, wrongN)));
            Assert.IsTrue(a.Conn.SendUdp(FrameEncoder.Encode(MsgType.GameIn, new byte[] { 0, 0, 0, 1, 7 }))); // axis 7
            byte[] badVersion = ClientMessages.GameIn(PongInput.Encode(1, PongIds.AxisUp));
            badVersion[0] = 2;
            Assert.IsTrue(a.Conn.SendUdp(badVersion));

            // 그 뒤 정상 입력은 그대로 쓰이고, TCP 연결도 살아 있다(ERROR 없이 PONG).
            ServerPeer.Run(a, p => p.SendInputUdp(PongIds.AxisUp), b, p => p.SendInput(), untilMs: 600);
            Assert.Greater(a.Latest.LastInputSeq, 0u, "valid UDP input after malformed ones is used");
            Assert.Less(a.Latest.Paddle0Y, Rules.field_h / 2, "paddle moved up");
            a.Send(ClientMessages.Ping(4321));
            Assert.AreEqual(4321u, PongReply.Parse(a.WaitFor(MsgType.Pong).Payload).ClientTimeMs);
            Assert.IsTrue(a.Conn.IsConnected);
        }
    }
}
