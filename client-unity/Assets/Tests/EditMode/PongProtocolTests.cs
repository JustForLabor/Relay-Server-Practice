using System.Collections.Generic;
using NUnit.Framework;
using Pong.Protocol;
using Relay.Net;

namespace Relay.Tests
{
    public class PongProtocolTests
    {
        [Test]
        public void DefaultSettings_MatchProtocolTable()
        {
            // protocol.md 6.1절 표의 순서 · 기본값: u16 12개 + u8 1개, big-endian.
            byte[] expected =
            {
                0x03, 0x20, // field_w 800
                0x02, 0x58, // field_h 600
                0x00, 0x0A, // paddle_w 10
                0x00, 0x64, // paddle_h 100
                0x00, 0x14, // paddle_inset 20
                0x00, 0x0C, // paddle_speed 12
                0x00, 0x0A, // ball_size 10
                0x00, 0x06, // serve_vx 6
                0x00, 0x03, // serve_vy 3
                0x00, 0x0E, // max_vx 14
                0x00, 0x08, // max_vy 8
                0x00, 0x1E, // serve_wait_ticks 30
                0x05,       // win_score 5
            };
            var s = new PongSettings();
            Assert.IsNull(s.Validate());
            byte[] b = s.ToBytes();
            Assert.AreEqual(25, b.Length);
            CollectionAssert.AreEqual(expected, b);

            var back = PongSettings.Parse(b);
            CollectionAssert.AreEqual(expected, back.ToBytes());
        }

        [Test]
        public void Settings_ValidateDependentRanges()
        {
            Assert.AreEqual("paddle_h 301 (10-300)", new PongSettings { paddle_h = 301 }.Validate());
            Assert.AreEqual("paddle_inset 9 (10-200)", new PongSettings { paddle_inset = 9 }.Validate());
            Assert.AreEqual("max_vx 21 (1-20)", new PongSettings { max_vx = 21 }.Validate());
            Assert.AreEqual("serve_vx 15 (1-14)", new PongSettings { serve_vx = 15 }.Validate());
            Assert.AreEqual("win_score 0 (1-21)", new PongSettings { win_score = 0 }.Validate());
            Assert.IsNull(new PongSettings { field_h = 4000, paddle_h = 2000 }.Validate());
        }

        [Test]
        public void State_DecodesProtocolExample()
        {
            // protocol.md 6.1절 "바이트 예시"의 STATE 프레임 그대로.
            byte[] frame =
            {
                0x01, 0xC0, 0x00, 0x14,
                0x01,
                0x00, 0x00, 0x00, 0x78,
                0x00, 0x00, 0x00, 0x29,
                0x01, 0x90, 0x01, 0x2C,
                0x01, 0x2C, 0x00, 0xFA,
                0x01, 0x00, 0x01,
            };
            var got = new List<Frame>();
            new FrameDecoder().Push(frame, 0, frame.Length, got.Add);
            Assert.AreEqual(1, got.Count);
            Assert.AreEqual(MsgType.GameOut, got[0].Type);

            var s = PongState.Parse(got[0].Payload);
            Assert.AreEqual(120u, s.Tick);
            Assert.AreEqual(41u, s.LastInputSeq);
            Assert.AreEqual(400, s.BallX);
            Assert.AreEqual(300, s.BallY);
            Assert.AreEqual(300, s.Paddle0Y);
            Assert.AreEqual(250, s.Paddle1Y);
            Assert.AreEqual(1, s.Score0);
            Assert.AreEqual(0, s.Score1);
            Assert.AreEqual(PongIds.PhasePlay, s.Phase);
            CollectionAssert.AreEqual(got[0].Payload, s.ToGameOut());
        }

        [Test]
        public void GameIn_MatchesProtocolExample()
        {
            // input_seq 42, 위 → 01 40 00 05 00 00 00 2A 01
            byte[] frame = ClientMessages.GameIn(PongInput.Encode(42, PongIds.AxisUp));
            CollectionAssert.AreEqual(new byte[] { 0x01, 0x40, 0x00, 0x05, 0x00, 0x00, 0x00, 0x2A, 0x01 }, frame);
        }

        [Test]
        public void Goal_Decodes()
        {
            var g = PongGoal.Parse(new byte[] { 0x02, 1, 0, 3 });
            Assert.AreEqual(1, g.ScorerSlot);
            Assert.AreEqual(0, g.Score0);
            Assert.AreEqual(3, g.Score1);
            Assert.Throws<ProtocolException>(() => PongGoal.Parse(new byte[] { 0x02, 1, 0 }));
            Assert.Throws<ProtocolException>(() => PongState.Parse(new byte[] { 0x02, 1, 0, 3 }));
        }

        [Test]
        public void BundleGameIn_Bytes_OldestFirst_UpToFour()
        {
            var h = new PongInputHistory();
            h.Add(41, PongIds.AxisNone);
            // n = 1 → 길이 6: 01 | 00 00 00 29 00
            CollectionAssert.AreEqual(new byte[] { 1, 0, 0, 0, 0x29, 0 }, h.EncodeBundle());

            h.Add(42, PongIds.AxisUp);
            h.Add(43, PongIds.AxisDown);
            h.Add(44, PongIds.AxisUp);
            byte[] four =
            {
                4,
                0, 0, 0, 0x29, 0,
                0, 0, 0, 0x2A, 1,
                0, 0, 0, 0x2B, 2,
                0, 0, 0, 0x2C, 1,
            };
            CollectionAssert.AreEqual(four, h.EncodeBundle());
            Assert.AreEqual(21, four.Length);

            // 다섯 번째가 들어오면 가장 오래된 41이 빠진다: 방금(45) + 직전 3개.
            h.Add(45, PongIds.AxisNone);
            byte[] b = h.EncodeBundle();
            Assert.AreEqual(21, b.Length);
            var parsed = PongInput.Parse(b);
            CollectionAssert.AreEqual(new uint[] { 42, 43, 44, 45 }, parsed.ConvertAll(e => e.seq));
            CollectionAssert.AreEqual(new byte[] { 1, 2, 1, 0 }, parsed.ConvertAll(e => e.axis));

            // 프레임 전체: 01 40 00 15 + 21바이트.
            byte[] frame = ClientMessages.GameIn(b);
            CollectionAssert.AreEqual(new byte[] { 0x01, 0x40, 0x00, 0x15 }, new[] { frame[0], frame[1], frame[2], frame[3] });

            h.Clear();
            Assert.AreEqual(0, h.Count);
            Assert.Throws<System.InvalidOperationException>(() => h.EncodeBundle());
        }

        [Test]
        public void GameInParse_AcceptsOnlySpecLengths()
        {
            Assert.AreEqual(1, PongInput.Parse(PongInput.Encode(7, PongIds.AxisDown)).Count);
            foreach (int len in new[] { 6, 11, 16, 21 })
            {
                var p = new byte[len];
                p[0] = (byte)((len - 1) / 5);
                Assert.AreEqual(p[0], PongInput.Parse(p).Count, $"len {len}");
            }
            // n이 길이와 맞지 않음, 길이가 표에 없음, axis 범위 밖 → 서버의 BAD_GAME_INPUT.
            Assert.Throws<ProtocolException>(() => PongInput.Parse(new byte[] { 3, 0, 0, 0, 1, 0, 0, 0, 0, 2, 0 }));
            var nFive = new byte[26];
            nFive[0] = 5; // 묶음은 최대 4개
            Assert.Throws<ProtocolException>(() => PongInput.Parse(nFive));
            Assert.Throws<ProtocolException>(() => PongInput.Parse(new byte[4]));
            Assert.Throws<ProtocolException>(() => PongInput.Parse(new byte[] { 0, 0, 0, 1, 3 }));
        }

        [Test]
        public void StateBuffer_DropsStateNotNewerThanReceived()
        {
            var buf = new Pong.Game.StateBuffer();
            buf.Clear(PongIds.TickHz);
            Assert.IsTrue(buf.Add(new PongState { Tick = 10, BallX = 100 }, 1.0));
            Assert.IsTrue(buf.Add(new PongState { Tick = 12, BallX = 120 }, 1.07));
            // UDP로 순서가 바뀌어 늦게 온 11, 두 번 온 12는 버린다.
            Assert.IsFalse(buf.Add(new PongState { Tick = 11, BallX = 110 }, 1.08));
            Assert.IsFalse(buf.Add(new PongState { Tick = 12, BallX = 999 }, 1.09));
            Assert.AreEqual(2, buf.Count);
            Assert.AreEqual(12u, buf.Latest.Tick);
            Assert.AreEqual(120, buf.Latest.BallX);
            Assert.IsTrue(buf.Add(new PongState { Tick = 13, BallX = 130 }, 1.10));

            // 새 경기(MATCH_START)는 tick이 0부터 다시 시작하므로 기준을 버린 뒤에는 0도 받는다.
            buf.Clear(PongIds.TickHz);
            Assert.IsTrue(buf.Add(new PongState { Tick = 0 }, 2.0));
        }
    }
}
