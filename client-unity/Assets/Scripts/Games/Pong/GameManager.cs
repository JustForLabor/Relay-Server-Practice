using Pong.Protocol;
using Relay.Lobby;
using Relay.Net;
using UnityEngine;
using UnityEngine.InputSystem;

namespace Pong.Game
{
    /// <summary>
    /// Pong 게임 부분(overview 7절). 규칙 값 13개를 Inspector에 노출해 방을 만들 때 보내고,
    /// 경기 중 진행 간격(1/30초)마다 GAME_IN을 보내며, 받은 STATE를 100ms 과거로 보간해 그린다.
    /// 접속 · 방 · 로비 화면은 같은 오브젝트의 LobbyController가 맡는다.
    /// 들어간 방의 실제 규칙은 ROOM_JOINED로 받은 값이다(남이 만든 방이면 Inspector 값과 다를 수 있다).
    /// </summary>
    [RequireComponent(typeof(LobbyController))]
    public sealed class GameManager : MonoBehaviour, IRoomGame
    {
        [Header("서버")]
        [Tooltip("릴레이 서버 주소. 명령줄 -relayHost가 있으면 그 값을 쓴다.")]
        public string serverHost = "127.0.0.1";
        [Tooltip("릴레이 서버 TCP 포트. 명령줄 -relayPort가 있으면 그 값을 쓴다.")]
        [Range(1, 65535)] public int serverPort = 7777;

        // protocol.md 6.1절 표와 같은 이름 · 기본값 · 범위. 범위가 다른 값에 달린 것(paddle_h ≤ field_h/2 등)은
        // [Range]로 나타낼 수 없어 넓게 두고 OnValidate에서 서버와 같은 순서로 좁힌다.
        [Header("Pong 규칙 값 (protocol.md 6.1절) — 방을 만들 때 서버로 보낸다")]
        [Tooltip("경기장 가로. 경기장 단위(1 = 기본 경기장의 1픽셀). 200–4000")]
        [Range(200, 4000)] public int field_w = 800;
        [Tooltip("경기장 세로. 150–4000")]
        [Range(150, 4000)] public int field_h = 600;
        [Tooltip("패들 폭. 2–100")]
        [Range(2, 100)] public int paddle_w = 10;
        [Tooltip("패들 높이. 10–field_h의 절반")]
        [Range(10, 2000)] public int paddle_h = 100;
        [Tooltip("경기장 옆 벽에서 패들 중심까지 거리. paddle_w–field_w의 1/4")]
        [Range(2, 1000)] public int paddle_inset = 20;
        [Tooltip("진행 한 번(1/30초)에 패들이 움직이는 거리. 기본 12 = 초당 360. 1–100")]
        [Range(1, 100)] public int paddle_speed = 12;
        [Tooltip("공 한 변. 2–100")]
        [Range(2, 100)] public int ball_size = 10;
        [Tooltip("서브 가로 속도(진행 한 번에). 1–max_vx")]
        [Range(1, 200)] public int serve_vx = 6;
        [Tooltip("서브 세로 속도. 0–max_vy")]
        [Range(0, 100)] public int serve_vy = 3;
        [Tooltip("가로 속도 상한. 패들을 뚫고 지나가지 않게 1–ball_size + paddle_w")]
        [Range(1, 200)] public int max_vx = 14;
        [Tooltip("세로 속도 상한. 0–100")]
        [Range(0, 100)] public int max_vy = 8;
        [Tooltip("득점 뒤 서브까지 멈추는 진행 수. 기본 30 = 1초. 0–300")]
        [Range(0, 300)] public int serve_wait_ticks = 30;
        [Tooltip("이 점수에 먼저 닿으면 승리. 1–21. 명령줄 -winScore가 있으면 그 값을 쓴다.")]
        [Range(1, 21)] public int win_score = 5;

        [Header("화면")]
        [Tooltip("경기장을 그릴 카메라")]
        [SerializeField] private Camera fieldCamera;
        [Tooltip("로비 · 점수판 UI를 만들 Canvas의 RectTransform")]
        [SerializeField] private RectTransform uiRoot;
        [Tooltip("받은 상태를 이만큼(초) 과거로 보간해 그린다. 지터를 흡수하는 여유")]
        [Range(0f, 0.3f)] public float interpolationDelay = 0.1f;

        private LobbyController _lobby;
        private PongView _view;
        private readonly StateBuffer _buffer = new StateBuffer();
        private PongSettings _rules = new PongSettings();
        private int _mySlot;
        private bool _inRoom;
        private bool _inMatch;
        private uint _inputSeq;
        private double _inputBase; // 0번째 GAME_IN 시각. n번째 = _inputBase + n × 진행 간격
        private double _tickInterval = 1.0 / PongIds.TickHz;
        private PongBot _bot;
        private int? _winScoreOverride;
        private int _states;
        private double _nextStateLog;
        private readonly bool[] _votes = new bool[2];
        // 입력이 얼마나 늦게 반영되나: STATE마다 (마지막으로 보낸 input_seq − 그 진행에 쓰인 last_input_seq).
        // 서버 큐에 쌓여 기다리는 입력 수 + 오가는 중인 입력 수라서, 보내는 간격이 진행 간격보다 짧으면 커진다.
        private long _lagSum;
        private long _lagMax;
        private int _lagSamples;

        public ushort GameId => PongIds.GameId;

        private void OnValidate()
        {
            paddle_h = Mathf.Clamp(paddle_h, 10, field_h / 2);
            paddle_inset = Mathf.Clamp(paddle_inset, paddle_w, field_w / 4);
            max_vx = Mathf.Clamp(max_vx, 1, ball_size + paddle_w);
            serve_vx = Mathf.Clamp(serve_vx, 1, max_vx);
            serve_vy = Mathf.Clamp(serve_vy, 0, max_vy);
        }

        private void Start()
        {
            Application.runInBackground = true; // 두 창을 띄우면 한쪽은 늘 포커스가 없다
            _lobby = GetComponent<LobbyController>();
            // 게임 전용 명령줄 옵션은 접속(Begin) 전에 읽는다: 자동 실행이 WELCOME 직후 방을 만들 때 -winScore가 이미 있어야 한다.
            var o = RunOptions.FromCommandLine();
            _winScoreOverride = o.GetInt("-winScore");
            if (o.Has("-bot"))
                _bot = new PongBot(System.Environment.TickCount);
            _view = new PongView(fieldCamera, uiRoot, OnRematchClicked, () => _lobby.LeaveRoom());
            _lobby.Begin(this, serverHost, serverPort, uiRoot);
        }

        /// <summary>Inspector 값(+ -winScore). 이 값이 25바이트 settings로 서버에 간다.</summary>
        public PongSettings InspectorSettings()
        {
            return new PongSettings
            {
                field_w = field_w, field_h = field_h, paddle_w = paddle_w, paddle_h = paddle_h,
                paddle_inset = paddle_inset, paddle_speed = paddle_speed, ball_size = ball_size,
                serve_vx = serve_vx, serve_vy = serve_vy, max_vx = max_vx, max_vy = max_vy,
                serve_wait_ticks = serve_wait_ticks, win_score = _winScoreOverride ?? win_score,
            };
        }

        public byte[] CreateSettings() => InspectorSettings().ToBytes();

        public string ValidateSettings() => InspectorSettings().Validate();

        // ---- 로비가 알려 주는 일 ----

        public void OnRoomJoined(RoomJoined joined)
        {
            _rules = PongSettings.Parse(joined.Settings);
            _mySlot = joined.MySlot;
            _inRoom = true;
            _inMatch = false;
            _buffer.Clear();
            _lobby.Log.Write($"room rules: {_rules}");
            _view.Setup(_rules, _mySlot);
            _view.SetFieldVisible(true);
            _view.HideResult();
            _view.SetBanner("");
            _view.DrawInitial();
        }

        public void OnPlayerJoined(int slot) => _view.SetBanner("");

        public void OnPlayerLeft(int slot)
        {
            // 결과 화면에서 상대가 나가면 다시 하기는 불가능하다(5.5절 3번). 로비가 대기 화면을 띄운다.
            _view.HideResult();
            _view.SetBanner("상대가 나갔습니다. 새 상대를 기다립니다");
        }

        public void OnMatchStart(uint startTick)
        {
            _inMatch = true;
            _inputSeq = 0;
            _inputBase = Time.realtimeSinceStartupAsDouble;
            // 진행 간격은 WELCOME의 게임 정보(tick_hz)를 따른다. 없으면 명세의 30Hz.
            int hz = _lobby.Game != null && _lobby.Game.TickHz > 0 ? _lobby.Game.TickHz : PongIds.TickHz;
            _tickInterval = 1.0 / hz;
            _states = 0;
            _lagSum = _lagMax = 0;
            _lagSamples = 0;
            _nextStateLog = Time.realtimeSinceStartupAsDouble + 5.0;
            _votes[0] = _votes[1] = false;
            _buffer.Clear(hz);
            _view.HideResult();
            _view.SetBanner("");
            _view.DrawInitial();
        }

        public void OnMatchEnd(byte reason)
        {
            _inMatch = false;
            _lobby.Log.Write($"match over: reason={reason} states={_states}");
            if (_lagSamples > 0)
                _lobby.Log.Write(string.Format(System.Globalization.CultureInfo.InvariantCulture,
                    "input lag (sent seq - STATE last_input_seq): samples={0} mean={1:F2} max={2}",
                    _lagSamples, _lagSum / (double)_lagSamples, _lagMax));
            if (reason != Relay.Net.Protocol.MatchEndByGame) // 그냥 Protocol은 이 이름공간에서 Pong.Protocol로 읽힌다
            {
                _view.SetBanner("상대가 나가 경기가 끝났습니다");
                return;
            }
            // 승리한 진행의 STATE(최종 점수)는 MATCH_END보다 먼저 온다(6.1절: GOAL → STATE → 끝).
            PongState last = _buffer.HasLatest ? _buffer.Latest : default;
            _view.ShowResult(last.Score0, last.Score1);
        }

        public void OnRematchVoted(int slot)
        {
            if (slot < 0 || slot > 1)
                return;
            _votes[slot] = true;
            if (_view.ResultVisible)
                _view.ShowVotes(_votes);
        }

        public void OnGameOut(byte[] payload)
        {
            if (payload.Length == 0)
                return;
            if (payload[0] == PongIds.OutState)
            {
                PongState s = PongState.Parse(payload);
                _buffer.Add(s, Time.realtimeSinceStartupAsDouble);
                _states++;
                if (_inMatch)
                {
                    long lag = (long)_inputSeq - s.LastInputSeq;
                    _lagSum += lag;
                    _lagSamples++;
                    if (lag > _lagMax)
                        _lagMax = lag;
                }
            }
            else if (payload[0] == PongIds.OutGoal)
            {
                PongGoal g = PongGoal.Parse(payload);
                _lobby.Log.Write($"GOAL scorer={g.ScorerSlot} score={g.Score0}:{g.Score1} states={_states}");
            }
            else
            {
                throw new ProtocolException($"unknown pong GAME_OUT kind 0x{payload[0]:X2}");
            }
        }

        public void OnLeftRoom()
        {
            _inRoom = false;
            _inMatch = false;
            _buffer.Clear();
            _view.HideResult();
            _view.SetFieldVisible(false);
        }

        private void OnRematchClicked() => _lobby.Rematch();

        // ---- 매 프레임 ----

        private void Update()
        {
            if (_inMatch)
                SendInputs();
            if (!_inRoom)
                return;
            if (_buffer.Sample(Time.realtimeSinceStartupAsDouble, interpolationDelay, out PongPose pose))
                _view.Draw(pose);
            if (_inMatch && Time.realtimeSinceStartupAsDouble >= _nextStateLog)
            {
                _nextStateLog += 5.0;
                _lobby.Log.Write($"STATE received={_states}");
            }
        }

        /// <summary>
        /// 진행 간격(1/30초)마다 하나(protocol.md 6.1절). n번째 전송 시각을 기준 시각 + n × 간격으로 계산해
        /// 프레임 시각의 반올림 오차가 쌓이지 않게 한다: 진행보다 조금이라도 빨리 보내면 서버 큐(4개)가 차서
        /// 입력이 그만큼 늦게 반영된다. 프레임이 길면 한 프레임에 밀린 만큼 보내되(최대 3개),
        /// 3개 넘게 밀렸으면 따라잡지 않고 기준 시각을 다시 잡는다: 몰아 보내도 서버 큐에서 버려진다.
        /// </summary>
        private void SendInputs()
        {
            double now = Time.realtimeSinceStartupAsDouble;
            if (now - (_inputBase + _inputSeq * _tickInterval) > _tickInterval * 3)
                _inputBase = now - _inputSeq * _tickInterval;
            int sent = 0;
            while (now >= _inputBase + _inputSeq * _tickInterval && sent < 3)
            {
                byte axis = ReadAxis();
                _lobby.SendGameIn(PongInput.Encode(++_inputSeq, axis));
                sent++;
            }
        }

        private byte ReadAxis()
        {
            if (_bot != null)
                return _buffer.HasLatest ? _bot.Decide(_buffer.Latest, _rules, _mySlot) : PongIds.AxisNone;
            Keyboard kb = Keyboard.current;
            if (kb == null)
                return PongIds.AxisNone;
            bool up = kb.upArrowKey.isPressed || kb.wKey.isPressed;
            bool down = kb.downArrowKey.isPressed || kb.sKey.isPressed;
            if (up == down)
                return PongIds.AxisNone; // 둘 다 누르거나 둘 다 안 누르면 정지
            return up ? PongIds.AxisUp : PongIds.AxisDown;
        }
    }
}
