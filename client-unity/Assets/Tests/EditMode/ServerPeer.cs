using System;
using System.Diagnostics;
using System.Threading;
using NUnit.Framework;
using Pong.Protocol;
using Relay.Net;

namespace Relay.Tests
{
    /// <summary>
    /// 실제 서버(127.0.0.1:7777, Docker)에 붙은 연결 하나와, 기다리는 동안 지나간 프레임을 세는 도우미.
    /// ServerTests(TCP)와 UdpServerTests(7장)가 같이 쓴다.
    /// </summary>
    internal sealed class ServerPeer
    {
        public const string Host = "127.0.0.1";
        public const int Port = 7777;
        public const int Wait = 5000;

        public readonly RelayConnection Conn = new RelayConnection();
        public readonly PongInputHistory History = new PongInputHistory();
        public readonly string Name;
        public int StatesTcp;
        public int StatesUdp;
        public int GoalsTcp;
        public int GoalsUdp;
        public uint InputSeq;
        /// <summary>지금까지 받은 STATE 중 tick이 가장 큰 것. UDP로는 순서가 바뀌어 올 수 있다.</summary>
        public PongState Latest;
        public bool HasState;

        public int States => StatesTcp + StatesUdp;
        public int Goals => GoalsTcp + GoalsUdp;

        private ServerPeer(string name) { Name = name; }

        /// <summary>접속하고 WELCOME을 확인한다. udp면 UDP_BOUND까지 기다린다(묶기는 2초 안에 끝나야 한다).</summary>
        public static ServerPeer Connect(string name, bool udp = false)
        {
            var p = new ServerPeer(name);
            p.Conn.Start(Host, Port, useUdp: udp);
            var sw = Stopwatch.StartNew();
            while (sw.ElapsedMilliseconds < Wait)
            {
                if (p.Conn.TryReceive(out RelayEvent e))
                {
                    if (e.Kind == RelayEventKind.Closed)
                        Assert.Fail($"{name}: connect to {Host}:{Port} failed: {e.Reason} (server running?)");
                    if (e.Kind == RelayEventKind.Connected)
                        break;
                }
                Thread.Sleep(2);
            }
            var w = Welcome.Parse(p.WaitFor(MsgType.Welcome).Payload);
            Assert.IsTrue(w.Games.Exists(g => g.GameId == PongIds.GameId && g.MinPlayers == 2 && g.TickHz == 30),
                "WELCOME must list Pong (game 1, 2 players, 30Hz)");
            if (udp)
                p.WaitUdpBound();
            return p;
        }

        private void WaitUdpBound()
        {
            var sw = Stopwatch.StartNew();
            while (sw.ElapsedMilliseconds < UdpDatagram.BindGiveUpMs + 1000)
            {
                if (!Conn.TryReceive(out RelayEvent e))
                {
                    Thread.Sleep(2);
                    continue;
                }
                if (e.Kind == RelayEventKind.UdpBound)
                    return;
                if (e.Kind == RelayEventKind.UdpFailed)
                    Assert.Fail($"{Name}: UDP bind failed: {e.Reason} (server UDP on 7777/udp?)");
                if (e.Kind == RelayEventKind.Closed)
                    Assert.Fail($"{Name}: closed while binding UDP: {e.Reason}");
                if (e.Kind == RelayEventKind.Frame)
                    Count(e);
            }
            Assert.Fail($"{Name}: no UdpBound/UdpFailed event in {UdpDatagram.BindGiveUpMs + 1000} ms");
        }

        public Frame WaitFor(byte type, int timeoutMs = Wait)
        {
            var sw = Stopwatch.StartNew();
            while (sw.ElapsedMilliseconds < timeoutMs)
            {
                if (!Conn.TryReceive(out RelayEvent e))
                {
                    Thread.Sleep(2);
                    continue;
                }
                if (e.Kind == RelayEventKind.Closed)
                    Assert.Fail($"{Name}: closed while waiting for 0x{type:X2}: {e.Reason}");
                if (e.Kind != RelayEventKind.Frame)
                    continue;
                Count(e);
                if (e.Frame.Type == type)
                    return e.Frame;
                if (e.Frame.Type == MsgType.Error && type != MsgType.Error)
                    Assert.Fail($"{Name}: ERROR {ErrorMessage.Parse(e.Frame.Payload)} while waiting for 0x{type:X2}");
            }
            Assert.Fail($"{Name}: timed out waiting for 0x{type:X2}");
            return default;
        }

        /// <summary>받은 GAME_OUT을 어느 길로 왔는지 나눠 센다.</summary>
        public void Count(RelayEvent e)
        {
            Frame f = e.Frame;
            if (f.Type != MsgType.GameOut)
                return;
            if (f.Payload[0] == PongIds.OutState)
            {
                if (e.ViaUdp) StatesUdp++; else StatesTcp++;
                PongState s = PongState.Parse(f.Payload);
                if (!HasState || s.Tick > Latest.Tick)
                {
                    Latest = s;
                    HasState = true;
                }
            }
            if (f.Payload[0] == PongIds.OutGoal)
            {
                if (e.ViaUdp) GoalsUdp++; else GoalsTcp++;
            }
        }

        public void Send(byte[] frame) => Assert.IsTrue(Conn.Send(frame), $"{Name}: send failed");

        /// <summary>TCP 단일 형식(6.1절).</summary>
        public void SendInput(byte axis = PongIds.AxisNone)
        {
            History.Add(++InputSeq, axis);
            Send(ClientMessages.GameIn(PongInput.Encode(InputSeq, axis)));
        }

        /// <summary>UDP 묶음 형식: 방금 입력 + 직전 3개(6.1절 · 7장).</summary>
        public void SendInputUdp(byte axis = PongIds.AxisNone)
        {
            History.Add(++InputSeq, axis);
            Assert.IsTrue(Conn.SendUdp(ClientMessages.GameIn(History.EncodeBundle())), $"{Name}: UDP send failed (bound?)");
        }

        /// <summary>MATCH_END를 받으면 true. 그 사이의 GAME_OUT은 센다.</summary>
        public bool Poll()
        {
            while (Conn.TryReceive(out RelayEvent e))
            {
                if (e.Kind == RelayEventKind.Closed)
                    Assert.Fail($"{Name}: closed during match: {e.Reason}");
                if (e.Kind != RelayEventKind.Frame)
                    continue;
                Count(e);
                if (e.Frame.Type == MsgType.MatchEnd)
                {
                    Assert.AreEqual(Protocol.MatchEndByGame, SimpleMessages.ParseU8(e.Frame.Payload), $"{Name}: MATCH_END reason");
                    return true;
                }
                if (e.Frame.Type == MsgType.Error)
                    Assert.Fail($"{Name}: ERROR {ErrorMessage.Parse(e.Frame.Payload)}");
            }
            return false;
        }

        /// <summary>
        /// 진행 간격(1000/30 ms)마다 둘 다 send를 부르며 untilMs가 지나거나(0이면 무시) 둘 다 MATCH_END를 받을 때까지 돈다.
        /// MATCH_END를 받은 쪽은 더 보내지 않는다. 둘 다 끝났으면 true.
        /// </summary>
        public static bool Run(ServerPeer a, Action<ServerPeer> sendA, ServerPeer b, Action<ServerPeer> sendB,
            int untilMs = 0, int limitMs = 30000)
        {
            var sw = Stopwatch.StartNew();
            bool aEnd = false, bEnd = false;
            double next = 0; // protocol.md 6.1절: 진행 간격(1000/30 ms)마다
            while (!(aEnd && bEnd))
            {
                if (untilMs > 0 && sw.ElapsedMilliseconds >= untilMs)
                    return false;
                Assert.Less(sw.ElapsedMilliseconds, limitMs, $"match did not end in {limitMs} ms");
                if (sw.Elapsed.TotalMilliseconds >= next)
                {
                    next += 1000.0 / PongIds.TickHz;
                    if (!aEnd) sendA(a);
                    if (!bEnd) sendB(b);
                }
                aEnd = aEnd || a.Poll();
                bEnd = bEnd || b.Poll();
                Thread.Sleep(1);
            }
            return true;
        }
    }
}
