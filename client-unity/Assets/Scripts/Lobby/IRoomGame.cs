using Relay.Net;

namespace Relay.Lobby
{
    /// <summary>
    /// 공통 로비(LobbyController)가 게임 부분에 넘기는 일. overview 7절의 나눔 그대로:
    /// 로비는 접속 · 방 · 경기 시작 · 끝까지만 알고, 규칙 값과 GAME_IN · GAME_OUT의 뜻은 게임이 정한다.
    /// 모든 호출은 Unity 메인 스레드에서 온다.
    /// </summary>
    public interface IRoomGame
    {
        ushort GameId { get; }

        /// <summary>방을 만들 때 보낼 settings 바이트. null이면 0바이트(서버 기본값).</summary>
        byte[] CreateSettings();

        /// <summary>보내기 전 검사. 문제가 있으면 사람이 읽을 문장, 없으면 null.</summary>
        string ValidateSettings();

        void OnRoomJoined(RoomJoined joined);
        void OnPlayerJoined(int slot);
        void OnPlayerLeft(int slot);
        void OnMatchStart(uint startTick);
        void OnMatchEnd(byte reason);
        void OnRematchVoted(int slot);
        void OnGameOut(byte[] payload);

        /// <summary>방에서 나왔다(나가기 · 연결 끊김). 화면을 지운다.</summary>
        void OnLeftRoom();
    }
}
