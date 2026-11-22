using System;
using System.Collections.Generic;
using NUnit.Framework;
using Relay.Net;

namespace Relay.Tests
{
    public class FrameTests
    {
        [Test]
        public void BigEndian_WritesHighByteFirst()
        {
            var b = new byte[6];
            BigEndian.WriteU16(b, 0, 0x1234);
            BigEndian.WriteU32(b, 2, 0xDEADBEEF);
            CollectionAssert.AreEqual(new byte[] { 0x12, 0x34, 0xDE, 0xAD, 0xBE, 0xEF }, b);
            Assert.AreEqual(0x1234, BigEndian.ReadU16(b, 0));
            Assert.AreEqual(0xDEADBEEFu, BigEndian.ReadU32(b, 2));
        }

        [Test]
        public void Encoder_BuildsHeader()
        {
            byte[] f = FrameEncoder.Encode(MsgType.Ping, new byte[] { 0, 0, 0, 7 });
            CollectionAssert.AreEqual(new byte[] { 1, 0x16, 0, 4, 0, 0, 0, 7 }, f);
            CollectionAssert.AreEqual(new byte[] { 1, 0x15, 0, 0 }, ClientMessages.LeaveRoom());
        }

        [Test]
        public void Encoder_RejectsOversizePayload()
        {
            Assert.Throws<ArgumentException>(() => FrameEncoder.Encode(MsgType.GameIn, new byte[513]));
        }

        private static byte[] SampleStream()
        {
            var parts = new List<byte>();
            parts.AddRange(FrameEncoder.Encode(MsgType.MatchStart, SimpleMessages.U32(0)));
            parts.AddRange(FrameEncoder.Encode(MsgType.PlayerJoined, SimpleMessages.U8(1)));
            parts.AddRange(FrameEncoder.Encode(MsgType.GameOut, new byte[500]));
            parts.AddRange(FrameEncoder.Encode(MsgType.Error, new ErrorMessage { Code = 1, Reason = "x" }.ToPayload()));
            return parts.ToArray();
        }

        private static void AssertSample(List<Frame> got)
        {
            Assert.AreEqual(4, got.Count);
            Assert.AreEqual(MsgType.MatchStart, got[0].Type);
            Assert.AreEqual(0u, SimpleMessages.ParseU32(got[0].Payload));
            Assert.AreEqual(MsgType.PlayerJoined, got[1].Type);
            Assert.AreEqual(1, SimpleMessages.ParseU8(got[1].Payload));
            Assert.AreEqual(MsgType.GameOut, got[2].Type);
            Assert.AreEqual(500, got[2].Payload.Length);
            Assert.AreEqual(MsgType.Error, got[3].Type);
            Assert.AreEqual("x", ErrorMessage.Parse(got[3].Payload).Reason);
        }

        [Test]
        public void Decoder_OneByteAtATime()
        {
            byte[] stream = SampleStream();
            var dec = new FrameDecoder();
            var got = new List<Frame>();
            for (int i = 0; i < stream.Length; i++)
                dec.Push(stream, i, 1, got.Add);
            AssertSample(got);
        }

        [Test]
        public void Decoder_AllAtOnce()
        {
            byte[] stream = SampleStream();
            var dec = new FrameDecoder();
            var got = new List<Frame>();
            dec.Push(stream, 0, stream.Length, got.Add);
            AssertSample(got);
        }

        [Test]
        public void Decoder_UnevenChunks()
        {
            byte[] stream = SampleStream();
            var dec = new FrameDecoder();
            var got = new List<Frame>();
            int[] sizes = { 3, 7, 1, 250, 2, 600 };
            int pos = 0;
            for (int i = 0; pos < stream.Length; i++)
            {
                int n = Math.Min(sizes[i % sizes.Length], stream.Length - pos);
                dec.Push(stream, pos, n, got.Add);
                pos += n;
            }
            AssertSample(got);
        }

        // 헤더 4바이트만 넣어도 바로 거부해야 한다(3장: 페이로드를 기다리지 않는다).
        [TestCase(new byte[] { 2, 0x80, 0, 9 }, TestName = "Decoder_RejectsVersion")]
        [TestCase(new byte[] { 1, 0x00, 0, 0 }, TestName = "Decoder_RejectsType0")]
        [TestCase(new byte[] { 1, 0x10, 0, 2 }, TestName = "Decoder_RejectsClientType")]
        [TestCase(new byte[] { 1, 0x89, 0, 1 }, TestName = "Decoder_RejectsUndefinedType")]
        [TestCase(new byte[] { 1, 0xC0, 0x02, 0x01 }, TestName = "Decoder_RejectsLenOver512")]
        [TestCase(new byte[] { 1, 0x85, 0, 3 }, TestName = "Decoder_RejectsWrongFixedLen")]
        [TestCase(new byte[] { 1, 0xC0, 0, 0 }, TestName = "Decoder_RejectsEmptyGameOut")]
        [TestCase(new byte[] { 1, 0x8F, 0, 67 }, TestName = "Decoder_RejectsLongError")]
        public void Decoder_RejectsBadHeader(byte[] header)
        {
            var dec = new FrameDecoder();
            int frames = 0;
            Assert.Throws<ProtocolException>(() => dec.Push(header, 0, header.Length, _ => frames++));
            Assert.AreEqual(0, frames);
        }

        [Test]
        public void Decoder_RejectsHeaderSplitAcrossPushes()
        {
            var dec = new FrameDecoder();
            var h = new byte[] { 1, 0xC0, 0x02, 0x01 };
            dec.Push(h, 0, 3, _ => { });
            Assert.Throws<ProtocolException>(() => dec.Push(h, 3, 1, _ => { }));
        }
    }
}
