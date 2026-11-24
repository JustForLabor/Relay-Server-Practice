using System.Diagnostics;
using Relay.Net;
using UnityEngine;

namespace Relay.Lobby
{
    public enum LobbyState
    {
        Offline,
        Connecting,
        Lobby,
        Room,
        Match,
    }

    /// <summary>
    /// 공통 로비 흐름(protocol.md 5장). 연결 하나를 갖고, 수신 큐를 메인 스레드 Update에서 비우며
    /// 공통 메시지는 여기서 처리하고 게임 메시지(GAME_OUT)와 방 · 경기 사건은 IRoomGame에 넘긴다.
    /// 다른 게임도 이 컴포넌트를 그대로 쓴다.
    /// </summary>
    [DisallowMultipleComponent]
    public sealed class LobbyController : MonoBehaviour
    {
        private const float PingInterval = 1f;

        private IRoomGame _game;
        private RelayConnection _conn;
        private LobbyView _view;
        private readonly Stopwatch _clock = Stopwatch.StartNew();
        private float _nextPing;
        private AutoRun _auto;

        public string Host { get; private set; }
        public int Port { get; private set; }
        public LobbyState State { get; private set; } = LobbyState.Offline;
        public RoomJoined Room { get; private set; }
        public int PlayerMask { get; private set; }
        /// <summary>마지막 PING 왕복 시간(ms). 아직 없으면 -1.</summary>
        public int RttMs { get; private set; } = -1;
        /// <summary>"경기가 막 끝난 방"(5.5절): 결과 화면을 보고 다시 하기를 고르는 중.</summary>
        public bool ResultPhase { get; private set; }
        public RunOptions Options { get; private set; }
        public RunLog Log { get; private set; }
        /// <summary>WELCOME에서 받은 이 게임의 정보(인원 · tick_hz). WELCOME 전이거나 서버에 없으면 null.</summary>
        public GameInfo Game { get; private set; }
        /// <summary>UDP를 시도하나(Inspector 값, 명령줄 -udp on|off가 있으면 그 값).</summary>
        public bool UseUdp { get; private set; }
        public UdpState Udp => _conn?.UdpState ?? UdpState.Off;
        /// <summary>묶였나. 게임은 이 값을 보고 GAME_IN 형식(UDP 묶음 · TCP 단일)을 고른다.</summary>
        public bool UdpBound => Udp == UdpState.Bound;
        /// <summary>지금 게임 데이터가 오가는 길. 상태 줄과 로그에 쓴다.</summary>
        public string Transport => UdpBound ? "UDP" : "TCP";

        /// <summary>
        /// 게임 쪽(GameManager)이 Start에서 부른다. 명령줄 -relayHost · -relayPort · -udp가 있으면 Inspector 값보다 앞선다.
        /// </summary>
        public void Begin(IRoomGame game, string host, int port, bool useUdp, RectTransform uiRoot)
        {
            _game = game;
            Options = RunOptions.FromCommandLine();
            Log = new RunLog(Options.Get("-log"));
            Host = Options.Get("-relayHost") ?? host;
            Port = Options.GetInt("-relayPort") ?? port;
            UseUdp = useUdp;
            string udp = Options.Get("-udp");
            if (udp != null)
            {
                if (string.Equals(udp, "on", System.StringComparison.OrdinalIgnoreCase))
                    UseUdp = true;
                else if (string.Equals(udp, "off", System.StringComparison.OrdinalIgnoreCase))
                    UseUdp = false;
                else
                    Log.Write($"-udp {udp}: expected on|off, keeping {(UseUdp ? "on" : "off")}");
            }
            _auto = new AutoRun(this, Options);
            _view = new LobbyView(this, uiRoot);
            Connect();
        }

        public void Connect()
        {
            _conn?.Close();
            DrainStale();
            _conn = new RelayConnection();
            State = LobbyState.Connecting;
            RttMs = -1;
            Log.Write($"connect {Host}:{Port} udp={(UseUdp ? "on" : "off")}");
            _conn.Start(Host, Port, useUdp: UseUdp);
        }

        // 이전 연결의 Closed 등이 새 연결 화면을 흔들지 않게 버린다.
        private void DrainStale()
        {
            if (_conn == null)
                return;
            while (_conn.TryReceive(out _)) { }
        }

        // ---- 보내기 (버튼 · 자동 실행 · 게임이 부른다) ----

        public void RequestRoomList() => Send(ClientMessages.ListRooms(_game.GameId));

        public void QuickJoin()
        {
            Log.Write("QUICK_JOIN");
            Send(ClientMessages.QuickJoin(_game.GameId));
        }

        public void CreateRoom(bool isPrivate)
        {
            string bad = _game.ValidateSettings();
            if (bad != null)
            {
                ShowError("규칙 값이 범위를 벗어남: " + bad);
                return;
            }
            byte vis = isPrivate ? Protocol.VisibilityPrivate : Protocol.VisibilityPublic;
            Log.Write($"CREATE_ROOM {(isPrivate ? "private" : "public")}");
            Send(ClientMessages.CreateRoom(_game.GameId, vis, _game.CreateSettings()));
        }

        public void JoinRoom(uint roomId)
        {
            Log.Write($"JOIN_ROOM {roomId}");
            Send(ClientMessages.JoinRoom(roomId));
        }

        public void JoinByCode(string raw)
        {
            string code = (raw ?? "").Trim().ToUpperInvariant();
            // 알파벳 밖 글자도 서버는 틀린 코드로 센다(5번이면 끊김). 오타로 횟수를 쓰지 않게 여기서 거른다.
            if (!Protocol.IsValidRoomCode(code))
            {
                ShowError("코드는 6글자, 0 · 1 · O · I를 뺀 대문자와 숫자입니다");
                return;
            }
            Log.Write($"JOIN_BY_CODE {code}");
            Send(ClientMessages.JoinByCode(code));
        }

        /// <summary>5.2절 7번: 서버는 답하지 않는다. 보낸 즉시 로비로 본다.</summary>
        public void LeaveRoom()
        {
            if (State != LobbyState.Room && State != LobbyState.Match)
                return;
            Log.Write("LEAVE_ROOM");
            Send(ClientMessages.LeaveRoom());
            ExitRoom();
            RequestRoomList();
        }

        public void Rematch()
        {
            Log.Write("REMATCH sent");
            Send(ClientMessages.Rematch());
        }

        /// <summary>TCP로 GAME_IN을 보낸다(게임이 정한 TCP 형식).</summary>
        public void SendGameIn(byte[] payload) => Send(ClientMessages.GameIn(payload));

        /// <summary>
        /// UDP로 GAME_IN을 보낸다(7장 묶기 4번). 묶이지 않았으면 false: 부른 쪽이 TCP 형식으로 다시 보낸다.
        /// UDP는 사라져도 되는 길이라 보내기 실패를 오류로 띄우지 않는다.
        /// </summary>
        public bool SendGameInUdp(byte[] payload) => _conn != null && _conn.SendUdp(ClientMessages.GameIn(payload));

        private void Send(byte[] frame)
        {
            if (_conn == null || !_conn.Send(frame))
                ShowError("서버에 연결되어 있지 않습니다");
        }

        public void ShowError(string text)
        {
            Log.Write("error: " + text);
            _view?.ShowError(text);
        }

        // ---- 받기 ----

        private void Update()
        {
            if (_conn == null)
                return;
            while (_conn != null && _conn.TryReceive(out RelayEvent e))
            {
                if (e.Kind == RelayEventKind.Connected)
                    Log.Write("connected");
                else if (e.Kind == RelayEventKind.Closed)
                    OnClosed(e.Reason);
                else if (e.Kind == RelayEventKind.UdpBound)
                    Log.Write("transport UDP: UDP_BOUND received");
                else if (e.Kind == RelayEventKind.UdpFailed)
                    Log.Write("transport TCP: UDP gave up: " + e.Reason);
                else
                    Dispatch(e.Frame);
            }

            if (_conn != null && _conn.IsConnected && Time.unscaledTime >= _nextPing)
            {
                _nextPing = Time.unscaledTime + PingInterval;
                _conn.Send(ClientMessages.Ping(NowMs()));
            }
            _auto.Tick();
            _view.Refresh();
        }

        private uint NowMs() => (uint)_clock.ElapsedMilliseconds;

        private void Dispatch(Frame f)
        {
            try
            {
                Handle(f);
            }
            catch (ProtocolException ex)
            {
                // 서버 바이트가 명세와 다르면 서버처럼 연결을 닫는다(5.4절의 "바로 끊는다"와 같은 태도).
                ShowError("서버 메시지 형식 오류: " + ex.Message);
                _conn.Close();
            }
        }

        private void Handle(Frame f)
        {
            switch (f.Type)
            {
                case MsgType.Welcome:
                    var w = Welcome.Parse(f.Payload);
                    Log.Write($"WELCOME conn_id={w.ConnId} games={w.Games.Count}");
                    State = LobbyState.Lobby;
                    Game = w.Games.Find(g => g.GameId == _game.GameId);
                    if (Game == null)
                        ShowError($"서버에 game_id {_game.GameId}가 없습니다");
                    else
                        Log.Write($"game {Game.Name}: {Game.MinPlayers}-{Game.MaxPlayers} players, tick_hz={Game.TickHz}");
                    RequestRoomList();
                    _auto.OnWelcome();
                    break;
                case MsgType.RoomList:
                    var list = RoomList.Parse(f.Payload);
                    if (list.GameId == _game.GameId)
                        _view.ShowRooms(list);
                    break;
                case MsgType.RoomJoined:
                    Room = RoomJoined.Parse(f.Payload);
                    PlayerMask = Room.PlayerMask;
                    State = LobbyState.Room;
                    ResultPhase = false;
                    Log.Write($"ROOM_JOINED room={Room.RoomId} slot={Room.MySlot} " +
                              $"{(Room.IsPrivate ? "private code=" + Room.Code : "public")} mask={Room.PlayerMask}");
                    _game.OnRoomJoined(Room);
                    break;
                case MsgType.PlayerJoined:
                    int joined = SimpleMessages.ParseU8(f.Payload);
                    PlayerMask |= 1 << joined;
                    Log.Write($"PLAYER_JOINED slot={joined}");
                    _game.OnPlayerJoined(joined);
                    break;
                case MsgType.PlayerLeft:
                    int left = SimpleMessages.ParseU8(f.Payload);
                    PlayerMask &= ~(1 << left);
                    ResultPhase = false; // 5.5절 3번: 동의가 지워지고 보통 대기 방이 된다
                    Log.Write($"PLAYER_LEFT slot={left}");
                    _game.OnPlayerLeft(left);
                    break;
                case MsgType.MatchStart:
                    uint startTick = SimpleMessages.ParseU32(f.Payload);
                    State = LobbyState.Match;
                    ResultPhase = false;
                    Log.Write($"MATCH_START start_tick={startTick}");
                    _game.OnMatchStart(startTick);
                    _auto.OnMatchStart();
                    break;
                case MsgType.MatchEnd:
                    byte reason = SimpleMessages.ParseU8(f.Payload);
                    if (State == LobbyState.Match)
                        State = LobbyState.Room;
                    ResultPhase = reason == Protocol.MatchEndByGame;
                    Log.Write($"MATCH_END reason={reason}");
                    _game.OnMatchEnd(reason);
                    _auto.OnMatchEnd(reason);
                    break;
                case MsgType.Pong:
                    var pong = PongReply.Parse(f.Payload);
                    RttMs = (int)(NowMs() - pong.ClientTimeMs);
                    break;
                case MsgType.RematchVoted:
                    int voter = SimpleMessages.ParseU8(f.Payload);
                    Log.Write($"REMATCH_VOTED slot={voter}");
                    _game.OnRematchVoted(voter);
                    break;
                case MsgType.Error:
                    var err = ErrorMessage.Parse(f.Payload);
                    ShowError(Describe(err));
                    break;
                case MsgType.GameOut:
                    _game.OnGameOut(f.Payload);
                    break;
            }
        }

        private void OnClosed(string reason)
        {
            Log.Write("disconnected: " + reason);
            bool wasInRoom = State == LobbyState.Room || State == LobbyState.Match;
            State = LobbyState.Offline;
            RttMs = -1;
            if (wasInRoom)
                ExitRoom();
            if (reason != "closed locally")
                ShowError("연결 끊김: " + reason);
        }

        private void ExitRoom()
        {
            if (State == LobbyState.Room || State == LobbyState.Match)
                State = LobbyState.Lobby;
            Room = null;
            PlayerMask = 0;
            ResultPhase = false;
            _game.OnLeftRoom();
        }

        private static string Describe(ErrorMessage e)
        {
            string text;
            switch (e.Kind)
            {
                case ErrorCode.BadState: text = "지금 할 수 없는 요청입니다"; break;
                case ErrorCode.UnknownGame: text = "서버에 없는 게임입니다"; break;
                case ErrorCode.RoomNotFound: text = "방이 없습니다(이미 사라졌거나 비공개 방)"; break;
                case ErrorCode.RoomFull: text = "방이 꽉 찼습니다"; break;
                case ErrorCode.BadCode: text = "입장 코드가 틀렸습니다"; break;
                case ErrorCode.TooManyCodeTries: text = "코드를 5번 틀려 연결이 끊깁니다"; break;
                case ErrorCode.BadGameInput: text = "서버가 게임 입력을 거부했습니다"; break;
                case ErrorCode.ServerFull: text = "서버 접속 인원이 가득 찼습니다"; break;
                case ErrorCode.BadSettings: text = "서버가 규칙 값을 거부했습니다"; break;
                default: text = "오류"; break;
            }
            return string.IsNullOrEmpty(e.Reason) ? $"{text} ({e.Code})" : $"{text} ({e.Code}: {e.Reason})";
        }

        private void OnDestroy()
        {
            _conn?.Close();
            Log?.Write("quit");
            Log?.Dispose();
        }
    }
}
