using System;
using System.Text;

namespace Relay.Net
{
    /// <summary>
    /// 페이로드를 앞에서부터 읽는다. 모자라면 ProtocolException: 서버가 길이를 잘못 보낸 것이므로
    /// 잘못된 값으로 화면을 그리기보다 연결을 닫는 편이 낫다.
    /// </summary>
    public sealed class PayloadReader
    {
        private readonly byte[] _b;
        private int _pos;

        public PayloadReader(byte[] payload)
        {
            _b = payload ?? Array.Empty<byte>();
        }

        public int Remaining => _b.Length - _pos;

        public byte U8()
        {
            Need(1);
            return _b[_pos++];
        }

        public ushort U16()
        {
            Need(2);
            ushort v = BigEndian.ReadU16(_b, _pos);
            _pos += 2;
            return v;
        }

        public uint U32()
        {
            Need(4);
            uint v = BigEndian.ReadU32(_b, _pos);
            _pos += 4;
            return v;
        }

        public byte[] Bytes(int n)
        {
            Need(n);
            var r = new byte[n];
            Buffer.BlockCopy(_b, _pos, r, 0, n);
            _pos += n;
            return r;
        }

        public string Utf8(int n)
        {
            Need(n);
            string s = Encoding.UTF8.GetString(_b, _pos, n);
            _pos += n;
            return s;
        }

        /// <summary>메시지 끝에서 부른다: 남는 바이트가 있으면 길이 필드와 내용이 어긋난 것이다.</summary>
        public void End(string what)
        {
            if (_pos != _b.Length)
                throw new ProtocolException($"{what}: {_b.Length - _pos} extra bytes");
        }

        private void Need(int n)
        {
            if (n < 0 || _pos + n > _b.Length)
                throw new ProtocolException($"payload too short: need {n} at {_pos}, have {_b.Length}");
        }
    }

    /// <summary>고정 크기 배열에 앞에서부터 쓴다. 크기는 메시지마다 미리 계산해 넘긴다.</summary>
    public sealed class PayloadWriter
    {
        private readonly byte[] _b;
        private int _pos;

        public PayloadWriter(int size)
        {
            _b = new byte[size];
        }

        public PayloadWriter U8(byte v)
        {
            _b[_pos++] = v;
            return this;
        }

        public PayloadWriter U16(ushort v)
        {
            BigEndian.WriteU16(_b, _pos, v);
            _pos += 2;
            return this;
        }

        public PayloadWriter U32(uint v)
        {
            BigEndian.WriteU32(_b, _pos, v);
            _pos += 4;
            return this;
        }

        public PayloadWriter Bytes(byte[] v)
        {
            if (v == null)
                return this;
            Buffer.BlockCopy(v, 0, _b, _pos, v.Length);
            _pos += v.Length;
            return this;
        }

        public byte[] Done()
        {
            if (_pos != _b.Length)
                throw new InvalidOperationException($"wrote {_pos} of {_b.Length} bytes");
            return _b;
        }
    }
}
