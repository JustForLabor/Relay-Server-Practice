using System.Collections.Generic;
using Pong.Protocol;

namespace Pong.Game
{
    /// <summary>보간해 그릴 한 순간. 좌표는 서버 좌표(실수).</summary>
    public struct PongPose
    {
        public float BallX;
        public float BallY;
        public float Paddle0Y;
        public float Paddle1Y;
        public int Score0;
        public int Score1;
    }

    /// <summary>
    /// 받은 STATE를 쌓아 두고 "지금보다 약 100ms 과거"를 두 상태 사이에서 보간한다(overview 7절).
    /// 수신 시각은 네트워크 지터로 들쭉날쭉하므로 시간축은 서버 진행 번호(tick, 30Hz)로 잡는다:
    /// 지금까지 가장 일찍 도착한 상태를 기준으로 "로컬 시각 ↔ tick" 관계를 정하면, 늦게 온 상태는 지연 여유(100ms) 안에서 흡수된다.
    /// </summary>
    public sealed class StateBuffer
    {
        private const int Keep = 60; // 2초. 100ms 지연이면 몇 개만 있으면 되지만 끊김 뒤 복구에 여유를 둔다
        private readonly List<PongState> _states = new List<PongState>();
        private double _offset = double.MaxValue; // 로컬 초 - tick/tick_hz 의 최솟값
        private int _tickHz = PongIds.TickHz;

        public int Count => _states.Count;
        public bool HasLatest => _states.Count > 0;
        public PongState Latest => _states[_states.Count - 1];

        /// <summary>방을 나가거나 새로 들어올 때: 쌓인 상태와 시간 기준을 버린다.</summary>
        public void Clear()
        {
            _states.Clear();
            _offset = double.MaxValue;
        }

        /// <summary>새 경기(MATCH_START): tick이 0부터 다시 시작하므로 기준을 버리고, 진행 간격(WELCOME의 tick_hz)을 정한다.</summary>
        public void Clear(int tickHz)
        {
            Clear();
            _tickHz = tickHz;
        }

        public void Add(PongState s, double now)
        {
            // TCP라 순서는 지켜지지만, 같은 tick이 두 번 오거나 거꾸로 오면 보간이 꼬이므로 버린다.
            if (_states.Count > 0 && s.Tick <= Latest.Tick)
                return;
            _states.Add(s);
            if (_states.Count > Keep)
                _states.RemoveAt(0);
            double candidate = now - s.Tick / (double)_tickHz;
            if (candidate < _offset)
                _offset = candidate;
        }

        /// <summary>now에서 delay초 과거의 모습. 상태가 없으면 false.</summary>
        public bool Sample(double now, double delay, out PongPose pose)
        {
            pose = default;
            if (_states.Count == 0)
                return false;
            double tick = (now - delay - _offset) * _tickHz;

            PongState a = _states[0], b = a;
            float t = 0f;
            if (tick <= _states[0].Tick)
            {
                a = b = _states[0];
            }
            else if (tick >= Latest.Tick)
            {
                a = b = Latest; // 예측(외삽)은 하지 않는다: 최신 상태에 멈춰 기다린다
            }
            else
            {
                for (int i = 1; i < _states.Count; i++)
                {
                    if (_states[i].Tick >= tick)
                    {
                        a = _states[i - 1];
                        b = _states[i];
                        t = (float)((tick - a.Tick) / (b.Tick - a.Tick));
                        break;
                    }
                }
            }

            // 득점한 진행에서 공은 가운데로 순간이동한다. 그 사이를 보간하면 공이 경기장을 가로질러 미끄러지므로
            // 점수가 바뀌는 구간은 앞 상태를 그대로 쓴다.
            bool goal = a.Score0 != b.Score0 || a.Score1 != b.Score1;
            pose.BallX = goal ? a.BallX : Lerp(a.BallX, b.BallX, t);
            pose.BallY = goal ? a.BallY : Lerp(a.BallY, b.BallY, t);
            pose.Paddle0Y = Lerp(a.Paddle0Y, b.Paddle0Y, t);
            pose.Paddle1Y = Lerp(a.Paddle1Y, b.Paddle1Y, t);
            PongState shown = t < 1f ? a : b;
            pose.Score0 = shown.Score0;
            pose.Score1 = shown.Score1;
            return true;
        }

        private static float Lerp(int x, int y, float t) => x + (y - x) * t;
    }
}
