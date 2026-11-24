using System;

namespace Relay.Net
{
    /// <summary>
    /// protocol.md 7장 데이터그램. 클라이언트 → 서버는 u32 udp_token + 프레임 하나, 서버 → 클라이언트는 프레임 하나.
    /// TCP의 FrameDecoder와 달리 데이터그램 경계가 곧 프레임 경계라 누적하지 않고, 어긋나면 예외 대신 false로 버린다:
    /// UDP는 출처를 믿을 수 없어 연결을 끊는 근거로 쓰지 않는다(7장).
    /// </summary>
    public static class UdpDatagram
    {
        public const int TokenLength = 4;
        /// <summary>UDP_BIND를 다시 보내는 간격과 포기 시간(7장 묶기 1번).</summary>
        public const int BindRetryMs = 200;
        public const int BindGiveUpMs = 2000;
        /// <summary>받기 버퍼: 가장 긴 정상 데이터그램(토큰 + 헤더 + 512)보다 크면 넘는 것을 잘린 채로 받아 길이로 걸러 낸다.</summary>
        public const int MaxDatagram = TokenLength + Protocol.HeaderLength + Protocol.MaxPayload;

        /// <summary>완성된 프레임(헤더 포함) 앞에 토큰을 붙인다.</summary>
        public static byte[] Build(uint token, byte[] frame)
        {
            if (frame == null || frame.Length < Protocol.HeaderLength)
                throw new ArgumentException("frame must include the 4-byte header");
            var buf = new byte[TokenLength + frame.Length];
            BigEndian.WriteU32(buf, 0, token);
            Buffer.BlockCopy(frame, 0, buf, TokenLength, frame.Length);
            return buf;
        }

        public static byte[] Bind(uint token) => Build(token, FrameEncoder.Encode(MsgType.UdpBind, null));

        /// <summary>
        /// 서버가 UDP로 보내는 것은 UDP_BOUND와 비신뢰 GAME_OUT뿐이다(7장 묶기 3번). 그 밖의 type은
        /// TCP로만 오가는 메시지이므로 UDP로 왔으면 어긋난 데이터그램으로 본다.
        /// </summary>
        public static bool TryParseServer(byte[] data, int count, out Frame frame)
        {
            frame = default;
            if (!TryHeader(data, 0, count, out byte type, out int len))
                return false;
            bool ok = type == MsgType.UdpBound ? len == 0
                : type == MsgType.GameOut && len >= 1 && len <= Protocol.MaxGamePayload;
            if (!ok)
                return false;
            var payload = new byte[len];
            Buffer.BlockCopy(data, Protocol.HeaderLength, payload, 0, len);
            frame = new Frame(type, payload);
            return true;
        }

        /// <summary>서버가 하는 해석(토큰 + 프레임 하나). 왕복 테스트와 테스트용 가짜 서버에 쓴다.</summary>
        public static bool TryParseClient(byte[] data, int count, out uint token, out Frame frame)
        {
            token = 0;
            frame = default;
            if (data == null || count < TokenLength)
                return false;
            if (!TryHeader(data, TokenLength, count - TokenLength, out byte type, out int len))
                return false;
            token = BigEndian.ReadU32(data, 0);
            var payload = new byte[len];
            Buffer.BlockCopy(data, TokenLength + Protocol.HeaderLength, payload, 0, len);
            frame = new Frame(type, payload);
            return true;
        }

        // 3장 프레임 규칙 중 데이터그램 하나만 보고 판단할 수 있는 것: 버전, 길이 상한, 길이와 실제 바이트 수가 같은가.
        private static bool TryHeader(byte[] data, int offset, int count, out byte type, out int len)
        {
            type = 0;
            len = 0;
            if (data == null || count < Protocol.HeaderLength || offset + count > data.Length)
                return false;
            if (data[offset] != Protocol.Version)
                return false;
            type = data[offset + 1];
            len = BigEndian.ReadU16(data, offset + 2);
            return len <= Protocol.MaxPayload && count == Protocol.HeaderLength + len;
        }
    }
}
