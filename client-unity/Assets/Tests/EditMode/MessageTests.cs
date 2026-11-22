using System.Collections.Generic;
using NUnit.Framework;
using Relay.Net;

namespace Relay.Tests
{
    /// <summary>4장 공통 메시지 전부를 프레임으로 만들고 다시 읽어 같은 값이 나오는지 본다.</summary>
    public class MessageTests
    {
        // 클라이언트 → 서버 프레임을 헤더와 페이로드로 나눈다(디코더는 서버 → 클라이언트 type만 받으므로 직접 자른다).
        private static byte[] Split(byte[] frame, byte expectType)
        {
            Assert.AreEqual(Protocol.Version, frame[0]);
            Assert.AreEqual(expectType, frame[1]);
            int len = BigEndian.ReadU16(frame, 2);
            Assert.AreEqual(frame.Length - 4, len);
            var p = new byte[len];
            System.Array.Copy(frame, 4, p, 0, len);
            return p;
        }

        // 서버 → 클라이언트 페이로드는 실제 수신 경로(FrameDecoder)를 지나게 한다.
        private static Frame ThroughDecoder(byte type, byte[] payload)
        {
            byte[] f = FrameEncoder.Encode(type, payload);
            var got = new List<Frame>();
            new FrameDecoder().Push(f, 0, f.Length, got.Add);
            Assert.AreEqual(1, got.Count);
            Assert.AreEqual(type, got[0].Type);
            return got[0];
        }

        [Test]
        public void Client_ListQuickJoin()
        {
            Assert.AreEqual(1, ClientMessages.ParseGameId(Split(ClientMessages.ListRooms(1), MsgType.ListRooms)));
            Assert.AreEqual(2, ClientMessages.ParseGameId(Split(ClientMessages.QuickJoin(2), MsgType.QuickJoin)));
        }

        [Test]
        public void Client_CreateRoom()
        {
            var settings = new byte[25];
            settings[24] = 9;
            byte[] p = Split(ClientMessages.CreateRoom(1, Protocol.VisibilityPrivate, settings), MsgType.CreateRoom);
            Assert.AreEqual(29, p.Length);
            ClientMessages.ParseCreateRoom(p, out ushort game, out byte vis, out byte[] s);
            Assert.AreEqual(1, game);
            Assert.AreEqual(1, vis);
            CollectionAssert.AreEqual(settings, s);

            p = Split(ClientMessages.CreateRoom(2, Protocol.VisibilityPublic, null), MsgType.CreateRoom);
            CollectionAssert.AreEqual(new byte[] { 0, 2, 0, 0 }, p);
        }

        [Test]
        public void Client_JoinRoomCodePing()
        {
            Assert.AreEqual(0xA1B2C3D4u, ClientMessages.ParseU32(Split(ClientMessages.JoinRoom(0xA1B2C3D4), MsgType.JoinRoom)));
            Assert.AreEqual("AB23XY", ClientMessages.ParseJoinByCode(Split(ClientMessages.JoinByCode("AB23XY"), MsgType.JoinByCode)));
            Assert.AreEqual(123456u, ClientMessages.ParseU32(Split(ClientMessages.Ping(123456), MsgType.Ping)));
            Assert.AreEqual(0, Split(ClientMessages.LeaveRoom(), MsgType.LeaveRoom).Length);
            Assert.AreEqual(0, Split(ClientMessages.Rematch(), MsgType.Rematch).Length);
            CollectionAssert.AreEqual(new byte[] { 9, 8 }, Split(ClientMessages.GameIn(new byte[] { 9, 8 }), MsgType.GameIn));
        }

        [Test]
        public void Server_Welcome()
        {
            var w = new Welcome { ConnId = 7, UdpToken = 0xCAFEBABE };
            w.Games.Add(new GameInfo { GameId = 1, MinPlayers = 2, MaxPlayers = 2, TickHz = 30, Name = "Pong" });
            w.Games.Add(new GameInfo { GameId = 2, MinPlayers = 2, MaxPlayers = 8, TickHz = 0, Name = "Relay" });
            byte[] p = w.ToPayload();
            Assert.AreEqual(9 + 7 + 4 + 7 + 5, p.Length);
            var r = Welcome.Parse(ThroughDecoder(MsgType.Welcome, p).Payload);
            Assert.AreEqual(7u, r.ConnId);
            Assert.AreEqual(0xCAFEBABEu, r.UdpToken);
            Assert.AreEqual(2, r.Games.Count);
            Assert.AreEqual("Pong", r.Games[0].Name);
            Assert.AreEqual(30, r.Games[0].TickHz);
            Assert.AreEqual(8, r.Games[1].MaxPlayers);
        }

        [Test]
        public void Server_RoomList()
        {
            var l = new RoomList { GameId = 1 };
            l.Rooms.Add(new RoomEntry { RoomId = 5, Players = 1, MaxPlayers = 2 });
            l.Rooms.Add(new RoomEntry { RoomId = 70000, Players = 1, MaxPlayers = 2 });
            var r = RoomList.Parse(ThroughDecoder(MsgType.RoomList, l.ToPayload()).Payload);
            Assert.AreEqual(1, r.GameId);
            Assert.AreEqual(2, r.Rooms.Count);
            Assert.AreEqual(70000u, r.Rooms[1].RoomId);
            Assert.Throws<ProtocolException>(() => RoomList.Parse(new byte[] { 0, 1, 2, 0, 0, 0, 5, 1, 2 }));
        }

        [Test]
        public void Server_RoomJoined()
        {
            var j = new RoomJoined
            {
                RoomId = 42, GameId = 1, MySlot = 1, MaxPlayers = 2, Visibility = 1, Code = "K7M2PQ",
                PlayerMask = 3, Settings = new byte[25],
            };
            var r = RoomJoined.Parse(ThroughDecoder(MsgType.RoomJoined, j.ToPayload()).Payload);
            Assert.AreEqual(42u, r.RoomId);
            Assert.AreEqual(1, r.MySlot);
            Assert.IsTrue(r.IsPrivate);
            Assert.AreEqual("K7M2PQ", r.Code);
            Assert.AreEqual(3, r.PlayerMask);
            Assert.AreEqual(25, r.Settings.Length);

            j.Visibility = 0;
            j.Code = null;
            j.Settings = new byte[0];
            byte[] p = j.ToPayload();
            Assert.AreEqual(17, p.Length);
            CollectionAssert.AreEqual(new byte[6], new[] { p[9], p[10], p[11], p[12], p[13], p[14] });
            Assert.IsNull(RoomJoined.Parse(p).Code);
        }

        [Test]
        public void Server_SmallMessages()
        {
            foreach (byte t in new[] { MsgType.PlayerJoined, MsgType.PlayerLeft, MsgType.MatchEnd, MsgType.RematchVoted })
                Assert.AreEqual(1, SimpleMessages.ParseU8(ThroughDecoder(t, SimpleMessages.U8(1)).Payload));
            Assert.AreEqual(99u, SimpleMessages.ParseU32(ThroughDecoder(MsgType.MatchStart, SimpleMessages.U32(99)).Payload));

            var pong = PongReply.Parse(ThroughDecoder(MsgType.Pong, new PongReply { ClientTimeMs = 1, ServerTimeMs = 2 }.ToPayload()).Payload);
            Assert.AreEqual(1u, pong.ClientTimeMs);
            Assert.AreEqual(2u, pong.ServerTimeMs);

            var err = ErrorMessage.Parse(ThroughDecoder(MsgType.Error, new ErrorMessage { Code = 9, Reason = "범위" }.ToPayload()).Payload);
            Assert.AreEqual(ErrorCode.BadSettings, err.Kind);
            Assert.AreEqual("범위", err.Reason);
            Assert.AreEqual(ErrorCode.BadState, ErrorMessage.Parse(new byte[] { 0, 1 }).Kind);
        }

        [Test]
        public void RoomCodeAlphabet()
        {
            Assert.IsTrue(Protocol.IsValidRoomCode("23ZXYW"));
            Assert.IsFalse(Protocol.IsValidRoomCode("O23ZXY"));
            Assert.IsFalse(Protocol.IsValidRoomCode("abcdef"));
            Assert.IsFalse(Protocol.IsValidRoomCode("ABCDE"));
        }
    }
}
