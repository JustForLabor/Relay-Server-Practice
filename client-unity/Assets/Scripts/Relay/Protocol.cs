using System;

namespace Relay.Net
{
    /// <summary>protocol.md 3–5장의 번호와 상한. 서버의 protocol.h와 같은 값이어야 한다.</summary>
    public static class Protocol
    {
        public const byte Version = 1;
        public const int HeaderLength = 4;
        public const int MaxPayload = 512;
        public const int MaxSettings = 64;
        public const int MaxGamePayload = 500;
        public const int MaxErrorReason = 64;
        public const int MaxRoomList = 32;
        public const int RoomCodeLength = 6;
        public const string RoomCodeAlphabet = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";

        public const byte VisibilityPublic = 0;
        public const byte VisibilityPrivate = 1;

        public const byte MatchEndByGame = 0;
        public const byte MatchEndTooFew = 1;

        /// <summary>
        /// 서버 → 클라이언트 type의 허용 길이(4장 표). 가변 길이 메시지는 범위만 보고,
        /// 내부 개수와 맞는지는 메시지 해석(ServerMessages)에서 본다.
        /// 헤더만 보고 판단할 수 있어야 페이로드를 기다리기 전에 끊을 수 있다(3장).
        /// </summary>
        public static bool TryGetServerLength(byte type, out int min, out int max)
        {
            switch (type)
            {
                case MsgType.Welcome: min = 9; max = MaxPayload; return true;
                case MsgType.RoomList: min = 3; max = 3 + 6 * MaxRoomList; return true;
                case MsgType.RoomJoined: min = 17; max = 17 + MaxSettings; return true;
                case MsgType.PlayerJoined:
                case MsgType.PlayerLeft:
                case MsgType.MatchEnd:
                case MsgType.RematchVoted: min = 1; max = 1; return true;
                case MsgType.MatchStart: min = 4; max = 4; return true;
                case MsgType.Pong: min = 8; max = 8; return true;
                case MsgType.Error: min = 2; max = 2 + MaxErrorReason; return true;
                case MsgType.GameOut: min = 1; max = MaxGamePayload; return true;
                default: min = 0; max = 0; return false;
            }
        }

        /// <summary>코드 입력란 검사용: 5.3절 알파벳 6글자인가. 대문자로 바꿔 보내는 것은 호출자 몫.</summary>
        public static bool IsValidRoomCode(string code)
        {
            if (code == null || code.Length != RoomCodeLength)
                return false;
            foreach (char c in code)
                if (RoomCodeAlphabet.IndexOf(c) < 0)
                    return false;
            return true;
        }
    }

    public static class MsgType
    {
        // 4.1 클라이언트 → 서버
        public const byte ListRooms = 0x10;
        public const byte CreateRoom = 0x11;
        public const byte QuickJoin = 0x12;
        public const byte JoinRoom = 0x13;
        public const byte JoinByCode = 0x14;
        public const byte LeaveRoom = 0x15;
        public const byte Ping = 0x16;
        public const byte Rematch = 0x17;
        public const byte UdpBind = 0x18;   // 7장: UDP로만
        public const byte GameIn = 0x40;

        // 4.2 서버 → 클라이언트
        public const byte Welcome = 0x80;
        public const byte RoomList = 0x81;
        public const byte RoomJoined = 0x82;
        public const byte PlayerJoined = 0x83;
        public const byte PlayerLeft = 0x84;
        public const byte MatchStart = 0x85;
        public const byte MatchEnd = 0x86;
        public const byte Pong = 0x87;
        public const byte RematchVoted = 0x88;
        public const byte UdpBound = 0x89;  // 7장: UDP로만
        public const byte Error = 0x8F;
        public const byte GameOut = 0xC0;
    }

    /// <summary>5.4절 오류 코드.</summary>
    public enum ErrorCode : ushort
    {
        BadState = 1,
        UnknownGame = 2,
        RoomNotFound = 3,
        RoomFull = 4,
        BadCode = 5,
        TooManyCodeTries = 6,
        BadGameInput = 7,
        ServerFull = 8,
        BadSettings = 9,
    }

    /// <summary>
    /// 서버가 명세와 다른 바이트를 보냈다. 서버가 같은 위반에 연결을 끊는 것처럼(5.4절)
    /// 클라이언트도 이 예외를 받으면 연결을 닫는다: 어긋난 스트림에서 다음 프레임 경계를 믿을 수 없다.
    /// </summary>
    public sealed class ProtocolException : Exception
    {
        public ProtocolException(string message) : base(message) { }
    }
}
