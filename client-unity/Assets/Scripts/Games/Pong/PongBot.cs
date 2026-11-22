using Pong.Protocol;

namespace Pong.Game
{
    /// <summary>
    /// -bot: 사람 없이 두 창 대전을 돌리기 위한 입력. 공 y를 따라 패들을 움직인다.
    /// 패들 속도(기본 12)가 공 세로 속도 상한(8)보다 빨라 그대로 따라가면 아무도 점수를 내지 못하고 경기가 끝나지 않는다.
    /// 그래서 공이 내 쪽으로 올 때마다 겨냥점을 패들 높이의 ±90% 안에서 무작위로 정한다:
    /// 겨냥점이 패들 반 높이 + 공 반 크기를 넘으면 놓친다(기본값에서 약 40%).
    /// </summary>
    public sealed class PongBot
    {
        private readonly System.Random _rng;
        private int _lastBallX = -1;
        private bool _incoming;
        private int _aim;

        public PongBot(int seed)
        {
            _rng = new System.Random(seed);
        }

        public byte Decide(PongState s, PongSettings rules, int mySlot)
        {
            bool incoming = _lastBallX >= 0 && (mySlot == 0 ? s.BallX < _lastBallX : s.BallX > _lastBallX);
            if (incoming && !_incoming)
            {
                int spread = rules.paddle_h * 9 / 10;
                _aim = _rng.Next(-spread, spread + 1);
            }
            _incoming = incoming;
            _lastBallX = s.BallX;

            int target = incoming ? s.BallY + _aim : rules.field_h / 2;
            int me = s.PaddleY(mySlot);
            int dead = rules.paddle_speed / 2 + 1; // 목표 근처에서 위아래로 떨지 않게
            if (me > target + dead)
                return PongIds.AxisUp; // 서버 y는 아래로 늘어나므로 위 = y 감소
            if (me < target - dead)
                return PongIds.AxisDown;
            return PongIds.AxisNone;
        }
    }
}
