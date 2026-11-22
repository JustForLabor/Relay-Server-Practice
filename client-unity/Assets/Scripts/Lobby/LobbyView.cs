using System.Collections.Generic;
using Relay.Net;
using UnityEngine;
using UnityEngine.UI;

namespace Relay.Lobby
{
    /// <summary>
    /// 로비 화면(접속 상태 · 방 목록 · 만들기 · 코드 입장)과 방 대기 화면. 경기 화면은 게임이 그린다.
    /// 상태는 LobbyController가 갖고, 여기서는 매 프레임 그 상태를 보여 주기만 한다.
    /// </summary>
    public sealed class LobbyView
    {
        private const float ErrorSeconds = 8f;

        private readonly LobbyController _lobby;
        private readonly Text _status;
        private readonly Text _error;
        private readonly RectTransform _lobbyPanel;
        private readonly RectTransform _roomPanel;
        private readonly Button _reconnect;
        private readonly RectTransform _roomList;
        private readonly Text _roomListEmpty;
        private readonly InputField _codeInput;
        private readonly Text _roomTitle;
        private readonly Text _roomCode;
        private readonly Text _roomPlayers;
        private readonly Text _roomInfo;
        private readonly List<GameObject> _roomRows = new List<GameObject>();
        private float _errorUntil;

        public LobbyView(LobbyController lobby, RectTransform root)
        {
            _lobby = lobby;

            _status = UiKit.Overlay(root, "Status", 16, TextAnchor.UpperLeft, new Vector2(0, 1), new Vector2(0.5f, 1));
            ((RectTransform)_status.transform).offsetMin = new Vector2(10, -34);
            _error = UiKit.Overlay(root, "Error", 18, TextAnchor.LowerCenter, new Vector2(0, 0), new Vector2(1, 0));
            ((RectTransform)_error.transform).offsetMax = new Vector2(-10, 40);
            _error.color = UiKit.ErrorColor;

            // 로비 패널
            _lobbyPanel = UiKit.Panel(root, "LobbyPanel", 600, new Vector2(0.5f, 0.5f));
            UiKit.Label(_lobbyPanel, "Relay Pong 로비", 28, TextAnchor.MiddleCenter);
            RectTransform row = UiKit.Row(_lobbyPanel, 40);
            UiKit.Button(row, "빠른 입장", lobby.QuickJoin);
            UiKit.Button(row, "공개 방 만들기", () => lobby.CreateRoom(false));
            UiKit.Button(row, "비공개 방 만들기", () => lobby.CreateRoom(true));

            row = UiKit.Row(_lobbyPanel, 40);
            _codeInput = UiKit.Input(row, "입장 코드 6글자", Protocol.RoomCodeLength);
            _codeInput.onValidateInput = (text, index, c) => char.ToUpperInvariant(c);
            UiKit.Width(UiKit.Button(row, "코드로 입장", () => lobby.JoinByCode(_codeInput.text)), 160);

            row = UiKit.Row(_lobbyPanel, 34);
            UiKit.Label(row, "공개 방 목록 (기다리는 방)", 18);
            UiKit.Width(UiKit.Button(row, "새로고침", lobby.RequestRoomList), 120);

            _roomList = UiKit.ScrollList(_lobbyPanel, 220);
            _roomListEmpty = UiKit.Label(_roomList, "기다리는 공개 방이 없습니다", 16, TextAnchor.MiddleCenter);
            _reconnect = UiKit.Button(_lobbyPanel, "다시 접속", lobby.Connect);

            // 방 대기 패널
            _roomPanel = UiKit.Panel(root, "RoomPanel", 520, new Vector2(0.5f, 0.5f));
            _roomTitle = UiKit.Label(_roomPanel, "", 24, TextAnchor.MiddleCenter);
            _roomCode = UiKit.Label(_roomPanel, "", 30, TextAnchor.MiddleCenter);
            _roomCode.color = new Color(1f, 0.85f, 0.3f);
            _roomPlayers = UiKit.Label(_roomPanel, "", 18, TextAnchor.MiddleCenter);
            _roomInfo = UiKit.Label(_roomPanel, "", 18, TextAnchor.MiddleCenter);
            UiKit.Button(_roomPanel, "나가기", lobby.LeaveRoom);
        }

        public void ShowError(string text)
        {
            _error.text = text;
            _errorUntil = Time.unscaledTime + ErrorSeconds;
        }

        public void ShowRooms(RoomList list)
        {
            foreach (var go in _roomRows)
                Object.Destroy(go);
            _roomRows.Clear();
            _roomListEmpty.gameObject.SetActive(list.Rooms.Count == 0);
            foreach (RoomEntry room in list.Rooms)
            {
                uint id = room.RoomId;
                Button b = UiKit.Button(_roomList, $"방 #{id}   {room.Players}/{room.MaxPlayers}명   입장", () => _lobby.JoinRoom(id));
                _roomRows.Add(b.gameObject);
            }
        }

        public void Refresh()
        {
            LobbyState s = _lobby.State;
            string rtt = _lobby.RttMs >= 0 ? $" · RTT {_lobby.RttMs} ms" : "";
            _status.text = $"{_lobby.Host}:{_lobby.Port} · {StateText(s)}{rtt}";

            bool inLobby = s == LobbyState.Offline || s == LobbyState.Connecting || s == LobbyState.Lobby;
            _lobbyPanel.gameObject.SetActive(inLobby);
            _reconnect.gameObject.SetActive(s == LobbyState.Offline);
            _roomPanel.gameObject.SetActive(s == LobbyState.Room && !_lobby.ResultPhase);
            if (_roomPanel.gameObject.activeSelf)
                FillRoom(_lobby.Room);

            if (_error.text.Length > 0 && Time.unscaledTime > _errorUntil)
                _error.text = "";
        }

        private void FillRoom(RoomJoined room)
        {
            if (room == null)
                return;
            _roomTitle.text = $"방 #{room.RoomId} · {(room.IsPrivate ? "비공개" : "공개")} · 내 자리 {room.MySlot}";
            _roomCode.gameObject.SetActive(room.IsPrivate);
            _roomCode.text = room.IsPrivate ? $"입장 코드  {room.Code}" : "";
            int count = 0;
            for (int i = 0; i < 8; i++)
                if ((_lobby.PlayerMask & (1 << i)) != 0)
                    count++;
            _roomPlayers.text = $"인원 {count}/{room.MaxPlayers}";
            _roomInfo.text = room.IsPrivate
                ? "친구에게 코드를 알려 주세요. 상대를 기다리는 중..."
                : "상대를 기다리는 중...";
        }

        private static string StateText(LobbyState s)
        {
            switch (s)
            {
                case LobbyState.Offline: return "연결 안 됨";
                case LobbyState.Connecting: return "접속 중...";
                case LobbyState.Lobby: return "로비";
                case LobbyState.Room: return "방에서 대기";
                default: return "경기 중";
            }
        }
    }
}
