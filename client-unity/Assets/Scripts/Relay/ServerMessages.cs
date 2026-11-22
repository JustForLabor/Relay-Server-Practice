using System;
using System.Collections.Generic;
using System.Text;

namespace Relay.Net
{
    // 4.2절 서버 → 클라이언트 메시지. Parse는 받은 페이로드를 해석하고,
    // ToPayload는 같은 바이트를 다시 만든다(왕복 테스트와 테스트용 가짜 서버 바이트에 쓴다).

    public sealed class GameInfo
    {
        public ushort GameId;
        public byte MinPlayers;
        public byte MaxPlayers;
        public ushort TickHz;
        public string Name;
    }

    public sealed class Welcome
    {
        public uint ConnId;
        public uint UdpToken;
        public readonly List<GameInfo> Games = new List<GameInfo>();

        public static Welcome Parse(byte[] p)
        {
            var r = new PayloadReader(p);
            var w = new Welcome { ConnId = r.U32(), UdpToken = r.U32() };
            int count = r.U8();
            for (int i = 0; i < count; i++)
            {
                var g = new GameInfo { GameId = r.U16(), MinPlayers = r.U8(), MaxPlayers = r.U8(), TickHz = r.U16() };
                g.Name = r.Utf8(r.U8());
                w.Games.Add(g);
            }
            r.End("WELCOME");
            return w;
        }

        public byte[] ToPayload()
        {
            int size = 9;
            var names = new List<byte[]>();
            foreach (var g in Games)
            {
                byte[] n = Encoding.UTF8.GetBytes(g.Name ?? "");
                names.Add(n);
                size += 7 + n.Length;
            }
            var w = new PayloadWriter(size).U32(ConnId).U32(UdpToken).U8((byte)Games.Count);
            for (int i = 0; i < Games.Count; i++)
            {
                var g = Games[i];
                w.U16(g.GameId).U8(g.MinPlayers).U8(g.MaxPlayers).U16(g.TickHz).U8((byte)names[i].Length).Bytes(names[i]);
            }
            return w.Done();
        }
    }

    public struct RoomEntry
    {
        public uint RoomId;
        public byte Players;
        public byte MaxPlayers;
    }

    public sealed class RoomList
    {
        public ushort GameId;
        public readonly List<RoomEntry> Rooms = new List<RoomEntry>();

        public static RoomList Parse(byte[] p)
        {
            var r = new PayloadReader(p);
            var l = new RoomList { GameId = r.U16() };
            int count = r.U8();
            if (count > Protocol.MaxRoomList)
                throw new ProtocolException($"ROOM_LIST count {count} > {Protocol.MaxRoomList}");
            for (int i = 0; i < count; i++)
                l.Rooms.Add(new RoomEntry { RoomId = r.U32(), Players = r.U8(), MaxPlayers = r.U8() });
            r.End("ROOM_LIST");
            return l;
        }

        public byte[] ToPayload()
        {
            var w = new PayloadWriter(3 + 6 * Rooms.Count).U16(GameId).U8((byte)Rooms.Count);
            foreach (var e in Rooms)
                w.U32(e.RoomId).U8(e.Players).U8(e.MaxPlayers);
            return w.Done();
        }
    }

    public sealed class RoomJoined
    {
        public uint RoomId;
        public ushort GameId;
        public byte MySlot;
        public byte MaxPlayers;
        public byte Visibility;
        /// <summary>비공개 방의 입장 코드. 공개 방이면 null(바이트는 0 여섯 개).</summary>
        public string Code;
        public byte PlayerMask;
        public byte[] Settings = Array.Empty<byte>();

        public bool IsPrivate => Visibility == Protocol.VisibilityPrivate;

        public static RoomJoined Parse(byte[] p)
        {
            var r = new PayloadReader(p);
            var j = new RoomJoined
            {
                RoomId = r.U32(), GameId = r.U16(), MySlot = r.U8(), MaxPlayers = r.U8(), Visibility = r.U8(),
            };
            byte[] code = r.Bytes(Protocol.RoomCodeLength);
            j.Code = Array.TrueForAll(code, b => b == 0) ? null : Encoding.ASCII.GetString(code);
            j.PlayerMask = r.U8();
            int n = r.U8();
            if (n > Protocol.MaxSettings)
                throw new ProtocolException($"ROOM_JOINED settings_len {n} > {Protocol.MaxSettings}");
            j.Settings = r.Bytes(n);
            r.End("ROOM_JOINED");
            return j;
        }

        public byte[] ToPayload()
        {
            var code = new byte[Protocol.RoomCodeLength];
            if (Code != null)
                Encoding.ASCII.GetBytes(Code, 0, Protocol.RoomCodeLength, code, 0);
            return new PayloadWriter(17 + Settings.Length)
                .U32(RoomId).U16(GameId).U8(MySlot).U8(MaxPlayers).U8(Visibility)
                .Bytes(code).U8(PlayerMask).U8((byte)Settings.Length).Bytes(Settings).Done();
        }
    }

    public sealed class PongReply
    {
        public uint ClientTimeMs;
        public uint ServerTimeMs;

        public static PongReply Parse(byte[] p)
        {
            var r = new PayloadReader(p);
            var m = new PongReply { ClientTimeMs = r.U32(), ServerTimeMs = r.U32() };
            r.End("PONG");
            return m;
        }

        public byte[] ToPayload() => new PayloadWriter(8).U32(ClientTimeMs).U32(ServerTimeMs).Done();
    }

    public sealed class ErrorMessage
    {
        public ushort Code;
        public string Reason = "";

        public ErrorCode Kind => (ErrorCode)Code;

        public static ErrorMessage Parse(byte[] p)
        {
            var r = new PayloadReader(p);
            var m = new ErrorMessage { Code = r.U16() };
            if (r.Remaining > Protocol.MaxErrorReason)
                throw new ProtocolException($"ERROR reason {r.Remaining} > {Protocol.MaxErrorReason}");
            m.Reason = r.Utf8(r.Remaining);
            return m;
        }

        public byte[] ToPayload()
        {
            byte[] reason = Encoding.UTF8.GetBytes(Reason ?? "");
            return new PayloadWriter(2 + reason.Length).U16(Code).Bytes(reason).Done();
        }

        public override string ToString() => $"{Kind}({Code}) {Reason}";
    }

    /// <summary>PLAYER_JOINED · PLAYER_LEFT · MATCH_END · REMATCH_VOTED · MATCH_START처럼 값 하나뿐인 메시지.</summary>
    public static class SimpleMessages
    {
        public static byte ParseU8(byte[] p)
        {
            var r = new PayloadReader(p);
            byte v = r.U8();
            r.End("u8");
            return v;
        }

        public static uint ParseU32(byte[] p)
        {
            var r = new PayloadReader(p);
            uint v = r.U32();
            r.End("u32");
            return v;
        }

        public static byte[] U8(byte v) => new[] { v };

        public static byte[] U32(uint v) => new PayloadWriter(4).U32(v).Done();
    }
}
