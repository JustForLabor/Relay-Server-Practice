using System;

namespace Relay.Net
{
    /// <summary>헤더를 벗긴 메시지 하나. Payload는 이 프레임만의 복사본이라 다른 스레드로 넘겨도 된다.</summary>
    public readonly struct Frame
    {
        public readonly byte Type;
        public readonly byte[] Payload;

        public Frame(byte type, byte[] payload)
        {
            Type = type;
            Payload = payload;
        }
    }

    public static class FrameEncoder
    {
        /// <summary>헤더 4바이트 + 페이로드를 한 배열로 만든다. 한 번의 Write로 보내야 다른 스레드의 송신과 섞이지 않는다.</summary>
        public static byte[] Encode(byte type, byte[] payload)
        {
            int len = payload == null ? 0 : payload.Length;
            if (len > Protocol.MaxPayload)
                throw new ArgumentException($"payload {len} > {Protocol.MaxPayload}");
            var buf = new byte[Protocol.HeaderLength + len];
            buf[0] = Protocol.Version;
            buf[1] = type;
            BigEndian.WriteU16(buf, 2, (ushort)len);
            if (len > 0)
                Buffer.BlockCopy(payload, 0, buf, Protocol.HeaderLength, len);
            return buf;
        }
    }

    /// <summary>
    /// TCP 바이트 흐름을 프레임으로 자르는 누적 디코더. TCP는 경계를 지키지 않으므로
    /// 한 번에 받은 바이트가 프레임 반쪽일 수도, 여러 개일 수도 있다.
    /// 헤더 4바이트가 모이는 즉시 버전 · type · 길이를 검사한다(3장: 512를 넘으면 페이로드를 기다리지 않는다).
    /// 한 스레드(수신 스레드)에서만 쓴다.
    /// </summary>
    public sealed class FrameDecoder
    {
        // 최대 프레임 하나(4 + 512)보다 크면 충분하다. 받은 만큼 앞으로 당겨 쓰므로 더 자라지 않는다.
        private readonly byte[] _buf = new byte[Protocol.HeaderLength + Protocol.MaxPayload];
        private int _count;
        private bool _headerChecked;

        /// <summary>
        /// 받은 바이트를 넣고, 완성된 프레임마다 onFrame을 부른다.
        /// 명세 위반이면 ProtocolException. 그 뒤로 이 디코더는 쓰지 않는다.
        /// </summary>
        public void Push(byte[] data, int offset, int count, Action<Frame> onFrame)
        {
            while (count > 0)
            {
                int need = Needed();
                int take = Math.Min(need, count);
                Buffer.BlockCopy(data, offset, _buf, _count, take);
                _count += take;
                offset += take;
                count -= take;

                if (_count >= Protocol.HeaderLength && !_headerChecked)
                {
                    CheckHeader();
                    _headerChecked = true;
                }
                if (_headerChecked && _count == Protocol.HeaderLength + PayloadLength())
                {
                    int len = PayloadLength();
                    var payload = new byte[len];
                    Buffer.BlockCopy(_buf, Protocol.HeaderLength, payload, 0, len);
                    byte type = _buf[1];
                    _count = 0;
                    _headerChecked = false;
                    onFrame(new Frame(type, payload));
                }
            }
        }

        /// <summary>지금 프레임을 마저 채우는 데 필요한 바이트 수. 헤더 전이면 헤더까지만 읽는다.</summary>
        private int Needed()
        {
            if (_count < Protocol.HeaderLength)
                return Protocol.HeaderLength - _count;
            return Protocol.HeaderLength + PayloadLength() - _count;
        }

        private int PayloadLength() => BigEndian.ReadU16(_buf, 2);

        private void CheckHeader()
        {
            byte version = _buf[0];
            byte type = _buf[1];
            int len = PayloadLength();
            if (version != Protocol.Version)
                throw new ProtocolException($"version {version} != {Protocol.Version}");
            if (len > Protocol.MaxPayload)
                throw new ProtocolException($"len {len} > {Protocol.MaxPayload}");
            if (!Protocol.TryGetServerLength(type, out int min, out int max))
                throw new ProtocolException($"unknown server type 0x{type:X2}");
            if (len < min || len > max)
                throw new ProtocolException($"type 0x{type:X2} len {len} not in {min}-{max}");
        }
    }
}
