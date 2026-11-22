using Relay.Net;

namespace Pong.Protocol
{
    /// <summary>protocol.md 6.1절 Pong의 번호들.</summary>
    public static class PongIds
    {
        public const ushort GameId = 1;
        public const int TickHz = 30;
        public const int SettingsLength = 25;

        public const byte OutState = 0x01;
        public const byte OutGoal = 0x02;
        public const int StateLength = 20;
        public const int GoalLength = 4;

        public const byte AxisNone = 0;
        public const byte AxisUp = 1;   // 서버 좌표에서 y가 줄어드는 쪽(화면 위)
        public const byte AxisDown = 2;

        public const byte PhaseServe = 0;
        public const byte PhasePlay = 1;
    }

    /// <summary>
    /// 방 규칙 값 13개(6.1절 표). 이름 · 순서 · 기본값은 표와 같다. 필드 이름을 표 그대로 두어
    /// 문서 · 서버(pong_settings) · Inspector에서 같은 이름으로 찾을 수 있게 했다.
    /// </summary>
    public sealed class PongSettings
    {
        public int field_w = 800;
        public int field_h = 600;
        public int paddle_w = 10;
        public int paddle_h = 100;
        public int paddle_inset = 20;
        public int paddle_speed = 12;
        public int ball_size = 10;
        public int serve_vx = 6;
        public int serve_vy = 3;
        public int max_vx = 14;
        public int max_vy = 8;
        public int serve_wait_ticks = 30;
        public int win_score = 5;

        public PongSettings Clone() => (PongSettings)MemberwiseClone();

        /// <summary>
        /// 범위를 벗어난 첫 필드를 "이름 값 (범위)"로 돌려준다. 맞으면 null.
        /// 서버(pong_settings_parse)와 같은 순서로 본다: 의존 범위(paddle_h ≤ field_h/2 등)는
        /// 앞에서 이미 통과한 값으로만 계산해야 서버와 판정이 같다.
        /// </summary>
        public string Validate()
        {
            return Check("field_w", field_w, 200, 4000)
                ?? Check("field_h", field_h, 150, 4000)
                ?? Check("paddle_w", paddle_w, 2, 100)
                ?? Check("paddle_h", paddle_h, 10, field_h / 2)
                ?? Check("paddle_inset", paddle_inset, paddle_w, field_w / 4)
                ?? Check("paddle_speed", paddle_speed, 1, 100)
                ?? Check("ball_size", ball_size, 2, 100)
                ?? Check("max_vx", max_vx, 1, ball_size + paddle_w)
                ?? Check("max_vy", max_vy, 0, 100)
                ?? Check("serve_vx", serve_vx, 1, max_vx)
                ?? Check("serve_vy", serve_vy, 0, max_vy)
                ?? Check("serve_wait_ticks", serve_wait_ticks, 0, 300)
                ?? Check("win_score", win_score, 1, 21);
        }

        private static string Check(string name, int v, int lo, int hi) =>
            v >= lo && v <= hi ? null : $"{name} {v} ({lo}-{hi})";

        /// <summary>
        /// 25바이트(u16 12개 + u8 1개, big-endian). 형에 담기지 않는 값은 잘라 넣지 않고 그대로 맞춰 넣는다:
        /// 범위 검사는 서버가 하며(BAD_SETTINGS), 범위 밖 값을 보내 오류를 확인하는 테스트도 이 경로를 쓴다.
        /// </summary>
        public byte[] ToBytes()
        {
            return new PayloadWriter(PongIds.SettingsLength)
                .U16((ushort)field_w).U16((ushort)field_h).U16((ushort)paddle_w).U16((ushort)paddle_h)
                .U16((ushort)paddle_inset).U16((ushort)paddle_speed).U16((ushort)ball_size)
                .U16((ushort)serve_vx).U16((ushort)serve_vy).U16((ushort)max_vx).U16((ushort)max_vy)
                .U16((ushort)serve_wait_ticks).U8((byte)win_score).Done();
        }

        /// <summary>ROOM_JOINED의 settings를 읽는다. 0바이트는 명세상 서버 기본값이다(서버는 항상 25바이트를 보낸다).</summary>
        public static PongSettings Parse(byte[] b)
        {
            if (b == null || b.Length == 0)
                return new PongSettings();
            if (b.Length != PongIds.SettingsLength)
                throw new ProtocolException($"pong settings length {b.Length} != {PongIds.SettingsLength}");
            var r = new PayloadReader(b);
            var s = new PongSettings
            {
                field_w = r.U16(), field_h = r.U16(), paddle_w = r.U16(), paddle_h = r.U16(),
                paddle_inset = r.U16(), paddle_speed = r.U16(), ball_size = r.U16(),
                serve_vx = r.U16(), serve_vy = r.U16(), max_vx = r.U16(), max_vy = r.U16(),
                serve_wait_ticks = r.U16(), win_score = r.U8(),
            };
            r.End("pong settings");
            return s;
        }

        public override string ToString() =>
            $"field {field_w}x{field_h} paddle {paddle_w}x{paddle_h} inset {paddle_inset} speed {paddle_speed} " +
            $"ball {ball_size} serve {serve_vx},{serve_vy} max {max_vx},{max_vy} wait {serve_wait_ticks} win {win_score}";
    }

    /// <summary>GAME_OUT STATE(종류 0x01, 20바이트). 좌표는 서버 좌표(원점 왼쪽 위, y 아래로 증가).</summary>
    public struct PongState
    {
        public uint Tick;
        public uint LastInputSeq;
        public int BallX;
        public int BallY;
        public int Paddle0Y;
        public int Paddle1Y;
        public int Score0;
        public int Score1;
        public byte Phase;

        public int PaddleY(int slot) => slot == 0 ? Paddle0Y : Paddle1Y;

        public static PongState Parse(byte[] gameOut)
        {
            var r = new PayloadReader(gameOut);
            if (r.U8() != PongIds.OutState)
                throw new ProtocolException("not a STATE");
            var s = new PongState
            {
                Tick = r.U32(), LastInputSeq = r.U32(),
                BallX = r.U16(), BallY = r.U16(), Paddle0Y = r.U16(), Paddle1Y = r.U16(),
                Score0 = r.U8(), Score1 = r.U8(), Phase = r.U8(),
            };
            r.End("STATE");
            return s;
        }

        public byte[] ToGameOut()
        {
            return new PayloadWriter(PongIds.StateLength).U8(PongIds.OutState).U32(Tick).U32(LastInputSeq)
                .U16((ushort)BallX).U16((ushort)BallY).U16((ushort)Paddle0Y).U16((ushort)Paddle1Y)
                .U8((byte)Score0).U8((byte)Score1).U8(Phase).Done();
        }
    }

    /// <summary>GAME_OUT GOAL(종류 0x02, 4바이트).</summary>
    public struct PongGoal
    {
        public int ScorerSlot;
        public int Score0;
        public int Score1;

        public static PongGoal Parse(byte[] gameOut)
        {
            var r = new PayloadReader(gameOut);
            if (r.U8() != PongIds.OutGoal)
                throw new ProtocolException("not a GOAL");
            var g = new PongGoal { ScorerSlot = r.U8(), Score0 = r.U8(), Score1 = r.U8() };
            r.End("GOAL");
            return g;
        }
    }

    public static class PongInput
    {
        /// <summary>GAME_IN 페이로드 5바이트: u32 input_seq, u8 axis.</summary>
        public static byte[] Encode(uint inputSeq, byte axis) =>
            new PayloadWriter(5).U32(inputSeq).U8(axis).Done();
    }
}
