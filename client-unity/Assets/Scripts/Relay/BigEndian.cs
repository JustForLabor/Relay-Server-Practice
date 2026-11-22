namespace Relay.Net
{
    /// <summary>
    /// protocol.md 2장: 모든 정수는 big-endian이다. BitConverter는 기계의 바이트 순서를 따르므로
    /// (Windows · x64는 little-endian) 쓰지 않고 바이트를 직접 옮긴다.
    /// </summary>
    public static class BigEndian
    {
        public static ushort ReadU16(byte[] b, int offset)
        {
            return (ushort)((b[offset] << 8) | b[offset + 1]);
        }

        public static uint ReadU32(byte[] b, int offset)
        {
            return ((uint)b[offset] << 24) | ((uint)b[offset + 1] << 16)
                 | ((uint)b[offset + 2] << 8) | b[offset + 3];
        }

        public static void WriteU16(byte[] b, int offset, ushort v)
        {
            b[offset] = (byte)(v >> 8);
            b[offset + 1] = (byte)v;
        }

        public static void WriteU32(byte[] b, int offset, uint v)
        {
            b[offset] = (byte)(v >> 24);
            b[offset + 1] = (byte)(v >> 16);
            b[offset + 2] = (byte)(v >> 8);
            b[offset + 3] = (byte)v;
        }
    }
}
