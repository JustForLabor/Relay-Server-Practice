using System.IO;
using Relay.Net;
using UnityEngine;

namespace Relay.Lobby
{
    /// <summary>
    /// 명령줄 자동 실행: -autoplay quick|create|private|joincode=CODE, -shot 경로 -shotAt 초 [-shotFrom start|match|matchend], -quitAfter 초, -autoRematch.
    /// 사람이 버튼을 누르는 것과 같은 LobbyController 메서드만 부른다. 옵션이 없으면 아무것도 하지 않는다.
    /// </summary>
    public sealed class AutoRun
    {
        private bool _quitting;
        private readonly LobbyController _lobby;
        private readonly string _autoplay;
        private readonly string _shotPath;
        private readonly float _shotDelay;
        private readonly string _shotFrom;
        private readonly float _quitAfter;
        private readonly bool _autoRematch;

        private bool _joined;
        private double _shotTime = -1;
        private bool _shotRequested;
        private bool _shotReported;
        private double _rematchTime = -1;

        public AutoRun(LobbyController lobby, RunOptions o)
        {
            _lobby = lobby;
            _autoplay = o.Get("-autoplay");
            _shotPath = o.Get("-shot");
            // -shotAt은 기본으로 첫 MATCH_START로부터의 초다: 앱 시작 기준이면 접속 · 상대 대기 시간에 따라 경기 전 화면이 찍힐 수 있다.
            // -shotFrom start는 앱 시작 기준(로비 · 대기 화면), -shotFrom matchend는 첫 MATCH_END(0) 기준(결과 화면).
            _shotDelay = o.GetFloat("-shotAt") ?? 3f;
            _shotFrom = (o.Get("-shotFrom") ?? "match").ToLowerInvariant();
            if (_shotPath != null && _shotFrom == "start")
                _shotTime = _shotDelay;
            _quitAfter = o.GetFloat("-quitAfter") ?? 0f;
            _autoRematch = o.Has("-autoRematch");
            if (_autoplay != null || _shotPath != null || _quitAfter > 0)
                lobby.Log.Write($"autorun autoplay={_autoplay ?? "-"} shot={_shotPath ?? "-"} shotAt={_shotDelay} " +
                                $"quitAfter={_quitAfter} autoRematch={_autoRematch}");
        }

        /// <summary>WELCOME 직후 한 번: 정해진 방법으로 방에 들어간다.</summary>
        public void OnWelcome()
        {
            if (_autoplay == null || _joined)
                return;
            _joined = true;
            string mode = _autoplay.ToLowerInvariant();
            if (mode == "quick")
                _lobby.QuickJoin();
            else if (mode == "create")
                _lobby.CreateRoom(false);
            else if (mode == "private")
                _lobby.CreateRoom(true);
            else if (mode.StartsWith("joincode="))
                _lobby.JoinByCode(_autoplay.Substring("joincode=".Length));
            else
                _lobby.ShowError("알 수 없는 -autoplay 값: " + _autoplay);
        }

        public void OnMatchStart()
        {
            if (_shotPath != null && _shotFrom == "match" && _shotTime < 0)
                _shotTime = Time.realtimeSinceStartupAsDouble + _shotDelay;
            _rematchTime = -1;
        }

        public void OnMatchEnd(byte reason)
        {
            if (_shotPath != null && _shotFrom == "matchend" && _shotTime < 0 && reason == Protocol.MatchEndByGame)
                _shotTime = Time.realtimeSinceStartupAsDouble + _shotDelay;
            // 결과 화면을 잠깐 보인 뒤 동의한다: 두 창이 같은 순간에 보내도 서버는 순서대로 처리한다.
            if (_autoRematch && reason == Protocol.MatchEndByGame)
                _rematchTime = Time.realtimeSinceStartupAsDouble + 2.0;
        }

        public void Tick()
        {
            double now = Time.realtimeSinceStartupAsDouble;
            if (_shotTime >= 0 && !_shotRequested && now >= _shotTime)
            {
                _shotRequested = true;
                string dir = Path.GetDirectoryName(Path.GetFullPath(_shotPath));
                if (!string.IsNullOrEmpty(dir))
                    Directory.CreateDirectory(dir);
                if (File.Exists(_shotPath))
                    File.Delete(_shotPath); // 지난 실행의 파일을 "찍힘"으로 착각하지 않게
                // 앱 안에서 찍는다: OS 캡처는 다른 창이 위에 있으면 그 창이 찍힌다.
                ScreenCapture.CaptureScreenshot(_shotPath);
                _lobby.Log.Write($"screenshot requested {_shotPath} state={_lobby.State}");
            }
            if (_shotRequested && !_shotReported && File.Exists(_shotPath))
            {
                _shotReported = true; // 파일은 프레임 끝에 비동기로 써지므로 생긴 뒤에 따로 남긴다
                _lobby.Log.Write($"screenshot written {_shotPath}");
            }
            if (_rematchTime >= 0 && now >= _rematchTime)
            {
                _rematchTime = -1;
                if (_lobby.ResultPhase)
                    _lobby.Rematch();
            }
            if (_quitAfter > 0 && !_quitting && now >= _quitAfter)
            {
                _quitting = true;
                _lobby.Log.Write($"quitAfter {_quitAfter}s reached");
                Application.Quit();
            }
        }
    }
}
