using System;
using System.Text;

namespace Relay.Net
{
    /// <summary>
    /// 4.1절 클라이언트 → 서버 메시지. Build*는 보낼 프레임 전체(헤더 포함)를 만든다.
    /// Parse*는 서버가 하는 해석을 C#으로 옮긴 것으로, 왕복 테스트와 잘못된 값을 보내기 전 검사에 쓴다.
    /// </summary>
    public static class ClientMessages
    {
        public static byte[] ListRooms(ushort gameId) =>
            FrameEncoder.Encode(MsgType.ListRooms, new PayloadWriter(2).U16(gameId).Done());

        public static byte[] QuickJoin(ushort gameId) =>
            FrameEncoder.Encode(MsgType.QuickJoin, new PayloadWriter(2).U16(gameId).Done());

        public static byte[] CreateRoom(ushort gameId, byte visibility, byte[] settings)
        {
            int n = settings == null ? 0 : settings.Length;
            if (n > Protocol.MaxSettings)
                throw new ArgumentException($"settings {n} > {Protocol.MaxSettings}");
            if (visibility > Protocol.VisibilityPrivate)
                throw new ArgumentException($"visibility {visibility}");
            var p = new PayloadWriter(4 + n).U16(gameId).U8(visibility).U8((byte)n).Bytes(settings).Done();
            return FrameEncoder.Encode(MsgType.CreateRoom, p);
        }

        public static byte[] JoinRoom(uint roomId) =>
            FrameEncoder.Encode(MsgType.JoinRoom, new PayloadWriter(4).U32(roomId).Done());

        /// <summary>
        /// 코드는 ASCII 6바이트로 그대로 보낸다. 알파벳 밖 글자도 막지 않는다:
        /// 서버가 틀린 코드로 세어 BAD_CODE로 답하는 것이 명세(5.3절)이고, 입력란 검사는 UI 몫이다.
        /// </summary>
        public static byte[] JoinByCode(string code)
        {
            if (code == null || code.Length != Protocol.RoomCodeLength)
                throw new ArgumentException("code must be 6 characters");
            byte[] ascii = Encoding.ASCII.GetBytes(code);
            return FrameEncoder.Encode(MsgType.JoinByCode, ascii);
        }

        public static byte[] LeaveRoom() => FrameEncoder.Encode(MsgType.LeaveRoom, null);

        public static byte[] Ping(uint clientTimeMs) =>
            FrameEncoder.Encode(MsgType.Ping, new PayloadWriter(4).U32(clientTimeMs).Done());

        public static byte[] Rematch() => FrameEncoder.Encode(MsgType.Rematch, null);

        /// <summary>게임 페이로드는 해석하지 않고 그대로 싣는다(1장: 게임 영역).</summary>
        public static byte[] GameIn(byte[] gamePayload)
        {
            if (gamePayload == null || gamePayload.Length < 1 || gamePayload.Length > Protocol.MaxGamePayload)
                throw new ArgumentException("GAME_IN payload must be 1-500 bytes");
            return FrameEncoder.Encode(MsgType.GameIn, gamePayload);
        }

        // ---- 해석 (서버 쪽과 같은 규칙) ----

        public static ushort ParseGameId(byte[] p)
        {
            var r = new PayloadReader(p);
            ushort id = r.U16();
            r.End("game_id");
            return id;
        }

        public static void ParseCreateRoom(byte[] p, out ushort gameId, out byte visibility, out byte[] settings)
        {
            var r = new PayloadReader(p);
            gameId = r.U16();
            visibility = r.U8();
            int n = r.U8();
            if (n > Protocol.MaxSettings)
                throw new ProtocolException($"settings_len {n} > {Protocol.MaxSettings}");
            settings = r.Bytes(n);
            r.End("CREATE_ROOM");
        }

        public static uint ParseU32(byte[] p)
        {
            var r = new PayloadReader(p);
            uint v = r.U32();
            r.End("u32");
            return v;
        }

        public static string ParseJoinByCode(byte[] p)
        {
            var r = new PayloadReader(p);
            string code = Encoding.ASCII.GetString(r.Bytes(Protocol.RoomCodeLength));
            r.End("JOIN_BY_CODE");
            return code;
        }
    }
}
