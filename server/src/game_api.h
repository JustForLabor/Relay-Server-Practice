/* game_api.h - 서버 공통 영역(코어)과 게임 모듈 사이의 약속.
 *
 * 코어는 접속 · 메시지 자르기 · 로비 · 방 · 진행 시계를 맡고, 게임 규칙은 모른다.
 * 게임 모듈은 game_def 하나를 내놓고, 플레이어에게는 game_host가 준 함수로만 보낸다.
 * 코어는 GAME_IN 페이로드를 해석하지 않고 on_input에 그대로 넘기며, 게임이 보내는
 * 바이트는 GAME_OUT 페이로드로 그대로 감싼다.
 *
 * 바이트 형식: docs/protocol.md 4장(GAME_IN, GAME_OUT), 6장(게임별 명세).
 * 이유: docs/adr/0002-game-interface.md.
 */
#ifndef GAME_API_H
#define GAME_API_H

#include <stddef.h>
#include <stdint.h>

/* 방 하나의 최대 인원. slot은 0 .. max_players-1. */
#define GAME_MAX_SLOTS 8

/* GAME_IN · GAME_OUT 페이로드 상한. 프레임 페이로드 상한(512)보다 작게 둔다. */
#define GAME_MAX_PAYLOAD 500

/* broadcast에서 아무도 빼지 않을 때. */
#define GAME_SLOT_NONE (-1)

/* 보내기 방식. 1차는 둘 다 TCP로 나가고, 2차에서 UNRELIABLE만 UDP로 바뀐다.
 * 게임은 메시지의 성격만 고르고 실제 전송 계층은 모른다. */
enum game_send_mode {
    GAME_SEND_RELIABLE = 0,   /* 반드시 도착해야 함: 득점, 경기 결과, Relay 데이터 */
    GAME_SEND_UNRELIABLE = 1, /* 최신 것만 중요함: 매 진행의 상태 */
};

/* 코어가 게임에게 주는 기능. 게임은 ctx를 건드리지 않고 그대로 넘긴다.
 * 이 함수들은 메인 루프 스레드에서, game_def 콜백 안에서만 부른다. */
typedef struct game_host {
    void *ctx;

    /* slot 한 명에게 GAME_OUT 하나를 보낸다. 비어 있는 slot이면 아무것도 하지 않는다.
     * len은 1 .. GAME_MAX_PAYLOAD. 받는 쪽 송신 버퍼가 넘치면 코어가 그 연결을 끊는다. */
    void (*send)(void *ctx, int slot, const uint8_t *data, size_t len,
                 enum game_send_mode mode);

    /* except_slot을 뺀 방의 모든 사람에게 보낸다. 다 보내려면 GAME_SLOT_NONE. */
    void (*broadcast)(void *ctx, int except_slot, const uint8_t *data, size_t len,
                      enum game_send_mode mode);

    /* 게임이 경기를 끝낸다. 코어는 현재 콜백이 돌아온 뒤 MATCH_END(0)를 보내고
     * match_destroy를 부른 다음 방을 닫는다. 같은 경기에서 두 번 불러도 한 번만 처리한다. */
    void (*match_over)(void *ctx);
} game_host;

/* 게임 하나의 정의. 게임 모듈은 이 구조체 하나를 const로 내놓는다. */
typedef struct game_def {
    uint16_t id;           /* protocol.md 1장의 game_id. 0은 쓰지 않는다 */
    const char *name;      /* WELCOME에 실리는 이름. UTF-8, 255바이트 이하 */
    uint8_t min_players;   /* 이 인원이 처음 차면 경기 시작. 1 이상 */
    uint8_t max_players;   /* min_players 이상, GAME_MAX_SLOTS 이하 */
    uint16_t tick_hz;      /* 진행 간격(초당 횟수). 0이면 on_tick을 부르지 않는다 */
    uint16_t max_input_len;/* GAME_IN 페이로드 상한. 1 .. GAME_MAX_PAYLOAD */

    /* 경기 시작 때 부른다. 경기 상태를 만들어 돌려준다(실패하면 NULL, 코어가 방을 닫는다).
     * host 포인터는 match_destroy까지 유효하다. slots[i]가 1이면 slot i에 사람이 있다. */
    void *(*match_create)(const game_host *host, const uint8_t slots[GAME_MAX_SLOTS]);

    /* 경기 상태를 해제한다. 이 안에서 host 함수를 부르지 않는다. */
    void (*match_destroy)(void *match);

    /* 경기 중 들어오고 나간 사람. 없어도 되면 NULL.
     * 인원이 min_players 아래로 떨어지는 퇴장은 on_leave 대신 match_destroy만 부른다. */
    void (*on_join)(void *match, int slot);
    void (*on_leave)(void *match, int slot);

    /* GAME_IN 하나. 코어가 길이(1 .. max_input_len)를 이미 검사했다.
     * 형식이 틀리면 음수를 돌려준다: 코어가 ERROR(BAD_GAME_INPUT)를 보내고 연결을 끊는다.
     * data는 이 호출 안에서만 유효하다. 보관하려면 복사한다. */
    int (*on_input)(void *match, int slot, const uint8_t *data, size_t len);

    /* 진행 한 번. tick은 MATCH_START의 start_tick부터 1씩 늘어난다. tick_hz가 0이면 NULL. */
    void (*on_tick)(void *match, uint32_t tick);
} game_def;

#endif /* GAME_API_H */
