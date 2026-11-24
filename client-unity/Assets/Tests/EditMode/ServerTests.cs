using System.Collections.Generic;
using NUnit.Framework;
using Pong.Protocol;
using Relay.Net;

namespace Relay.Tests
{
    /// <summary>
    /// 실제 서버(127.0.0.1:7777, Docker)에 RelayConnection 두 개로 붙어 흐름 전체를 확인한다.
    /// 서버가 없으면 실패한다: 이 테스트의 목적이 서버와 바이트가 맞는지 보는 것이라 건너뛰면 의미가 없다.
    /// </summary>
    [Category("Server")]
    public class ServerTests
    {
        private readonly List<ServerPeer> _peers = new List<ServerPeer>();

        private ServerPeer Connect(string name)
        {
            ServerPeer p = ServerPeer.Connect(name);
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

        private static byte[] FastSettings()
        {
            // 첫 득점으로 끝나게: 기본 서브는 패들을 빗나가므로 입력 없이도 몇 초 안에 끝난다.
            return new PongSettings { win_score = 1, serve_wait_ticks = 5 }.ToBytes();
        }

        /// <summary>둘 다 진행 간격(1000/30 ms)마다 TCP GAME_IN을 보내며 MATCH_END를 기다린다.</summary>
        private static void PlayUntilEnd(ServerPeer a, ServerPeer b) =>
            ServerPeer.Run(a, p => p.SendInput(), b, p => p.SendInput());

        [Test, Timeout(60000)]
        public void PublicRoom_FullMatch_ThenRematch()
        {
            ServerPeer a = Connect("A"), b = Connect("B");
            byte[] settings = FastSettings();

            a.Send(ClientMessages.CreateRoom(PongIds.GameId, Protocol.VisibilityPublic, settings));
            var ja = RoomJoined.Parse(a.WaitFor(MsgType.RoomJoined).Payload);
            Assert.AreEqual(0, ja.MySlot);
            Assert.IsFalse(ja.IsPrivate);
            Assert.IsNull(ja.Code);
            CollectionAssert.AreEqual(settings, ja.Settings, "room must keep the settings we sent");

            b.Send(ClientMessages.ListRooms(PongIds.GameId));
            var list = RoomList.Parse(b.WaitFor(MsgType.RoomList).Payload);
            Assert.IsTrue(list.Rooms.Exists(r => r.RoomId == ja.RoomId && r.Players == 1 && r.MaxPlayers == 2), "room in list");

            b.Send(ClientMessages.JoinRoom(ja.RoomId));
            var jb = RoomJoined.Parse(b.WaitFor(MsgType.RoomJoined).Payload);
            Assert.AreEqual(ja.RoomId, jb.RoomId);
            Assert.AreEqual(1, jb.MySlot);
            Assert.AreEqual(3, jb.PlayerMask);
            CollectionAssert.AreEqual(settings, jb.Settings);
            Assert.AreEqual(1, SimpleMessages.ParseU8(a.WaitFor(MsgType.PlayerJoined).Payload));

            Assert.AreEqual(0u, SimpleMessages.ParseU32(a.WaitFor(MsgType.MatchStart).Payload));
            Assert.AreEqual(0u, SimpleMessages.ParseU32(b.WaitFor(MsgType.MatchStart).Payload));

            PlayUntilEnd(a, b);
            Assert.Greater(a.States, 10, "A STATE count");
            Assert.Greater(b.States, 10, "B STATE count");
            Assert.AreEqual(1, a.Goals);
            Assert.AreEqual(1, b.Goals);

            // 5.5절: 둘 다 동의하면 같은 방에서 다시 시작한다.
            a.Send(ClientMessages.Rematch());
            Assert.AreEqual(0, SimpleMessages.ParseU8(a.WaitFor(MsgType.RematchVoted).Payload));
            Assert.AreEqual(0, SimpleMessages.ParseU8(b.WaitFor(MsgType.RematchVoted).Payload));
            b.Send(ClientMessages.Rematch());
            Assert.AreEqual(1, SimpleMessages.ParseU8(a.WaitFor(MsgType.RematchVoted).Payload));
            Assert.AreEqual(1, SimpleMessages.ParseU8(b.WaitFor(MsgType.RematchVoted).Payload));
            Assert.AreEqual(0u, SimpleMessages.ParseU32(a.WaitFor(MsgType.MatchStart).Payload));
            Assert.AreEqual(0u, SimpleMessages.ParseU32(b.WaitFor(MsgType.MatchStart).Payload));

            // 새 경기도 실제로 진행되는지: STATE가 다시 온다.
            int before = a.States;
            a.SendInput();
            a.WaitFor(MsgType.GameOut);
            Assert.Greater(a.States, before);

            a.Send(ClientMessages.LeaveRoom());
            Assert.AreEqual(Protocol.MatchEndTooFew, SimpleMessages.ParseU8(b.WaitFor(MsgType.MatchEnd).Payload));
        }

        [Test, Timeout(30000)]
        public void PrivateRoom_JoinByCode()
        {
            ServerPeer a = Connect("A"), b = Connect("B");
            a.Send(ClientMessages.CreateRoom(PongIds.GameId, Protocol.VisibilityPrivate, null));
            var ja = RoomJoined.Parse(a.WaitFor(MsgType.RoomJoined).Payload);
            Assert.IsTrue(ja.IsPrivate);
            Assert.IsTrue(Protocol.IsValidRoomCode(ja.Code), $"code '{ja.Code}'");
            CollectionAssert.AreEqual(new PongSettings().ToBytes(), ja.Settings, "settings_len 0 → server defaults");

            // 틀린 코드는 BAD_CODE, 연결은 유지된다.
            string wrong = ja.Code == "222222" ? "333333" : "222222";
            b.Send(ClientMessages.JoinByCode(wrong));
            Assert.AreEqual(ErrorCode.BadCode, ErrorMessage.Parse(b.WaitFor(MsgType.Error).Payload).Kind);

            b.Send(ClientMessages.JoinByCode(ja.Code));
            var jb = RoomJoined.Parse(b.WaitFor(MsgType.RoomJoined).Payload);
            Assert.AreEqual(ja.RoomId, jb.RoomId);
            Assert.AreEqual(1, jb.MySlot);
            a.WaitFor(MsgType.MatchStart);
            b.WaitFor(MsgType.MatchStart);
        }

        [Test, Timeout(30000)]
        public void BadSettings_ErrorAndConnectionKept()
        {
            ServerPeer a = Connect("A");
            var s = new PongSettings { paddle_h = 400 }; // field_h 600의 절반(300)을 넘는다
            Assert.IsNotNull(s.Validate());
            a.Send(ClientMessages.CreateRoom(PongIds.GameId, Protocol.VisibilityPublic, s.ToBytes()));
            var err = ErrorMessage.Parse(a.WaitFor(MsgType.Error).Payload);
            Assert.AreEqual(ErrorCode.BadSettings, err.Kind);

            a.Send(ClientMessages.Ping(1234));
            var pong = PongReply.Parse(a.WaitFor(MsgType.Pong).Payload);
            Assert.AreEqual(1234u, pong.ClientTimeMs);
        }
    }
}
