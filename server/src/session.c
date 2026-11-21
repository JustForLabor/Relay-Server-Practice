/* session.c - 상태별 허용 메시지 검사와 공통 메시지 처리. */
#include "session.h"

#include "match.h"
#include "msg.h"
#include "room.h"
#include "tick.h"
#include "wire.h"

session_state session_state_of(const conn *c)
{
    if (c->room == NULL)
        return SESSION_LOBBY;
    return c->room->match != NULL ? SESSION_PLAYING : SESSION_WAITING;
}

void session_open(conn *c)
{
    msg_welcome(c);
}

static const char *error_text(int code)
{
    switch (code) {
    case ERR_BAD_STATE: return "bad state";
    case ERR_UNKNOWN_GAME: return "unknown game";
    case ERR_ROOM_NOT_FOUND: return "room not found";
    case ERR_ROOM_FULL: return "room full";
    case ERR_BAD_CODE: return "bad code";
    case ERR_TOO_MANY_CODE_TRIES: return "too many code tries";
    case ERR_BAD_GAME_INPUT: return "bad game input";
    case ERR_SERVER_FULL: return "server full";
    case ERR_BAD_SETTINGS: return "bad settings";
    default: return "error";
    }
}

/* 오류 코드를 ERROR로 답한다. 5.4절에서 "종료"인 코드는 답한 뒤 연결을 끊는다. */
static void reply_error(conn *c, int code)
{
    if (code == 0)
        return;
    msg_error(c, (uint16_t)code, error_text(code));
    if (code == ERR_TOO_MANY_CODE_TRIES || code == ERR_BAD_GAME_INPUT || code == ERR_SERVER_FULL)
        conn_kill(c, error_text(code));
}

static void handle_lobby_msg(conn *c, const frame *f)
{
    const uint8_t *p = f->payload;
    switch (f->type) {
    case MSG_LIST_ROOMS:
        reply_error(c, room_send_list(c, wire_get_u16(p)));
        break;
    case MSG_CREATE_ROOM:
        /* visibility는 0 · 1뿐이다. 다른 값은 길이 위반처럼 형식이 깨진 메시지로 보고 끊는다. */
        if (p[2] > ROOM_VISIBILITY_PRIVATE) {
            conn_kill(c, "bad visibility");
            return;
        }
        reply_error(c, room_create(c, wire_get_u16(p), p[2], p + 4, p[3]));
        break;
    case MSG_QUICK_JOIN:
        reply_error(c, room_quick_join(c, wire_get_u16(p)));
        break;
    case MSG_JOIN_ROOM:
        reply_error(c, room_join_by_id(c, wire_get_u32(p)));
        break;
    case MSG_JOIN_BY_CODE:
        reply_error(c, room_join_by_code(c, p));
        break;
    default:
        break;
    }
}

void session_handle(conn *c, const frame *f)
{
    session_state st = session_state_of(c);
    switch (f->type) {
    case MSG_PING: {
        /* 서버 시각은 단조 시계의 ms 하위 32비트. 클라이언트는 차이만 쓰므로 기준점은 상관없다. */
        uint32_t server_ms = (uint32_t)(tick_now_ns() / NS_PER_MS);
        msg_pong(c, wire_get_u32(f->payload), server_ms);
        return;
    }
    case MSG_LIST_ROOMS:
    case MSG_CREATE_ROOM:
    case MSG_QUICK_JOIN:
    case MSG_JOIN_ROOM:
    case MSG_JOIN_BY_CODE:
        if (st != SESSION_LOBBY)
            reply_error(c, ERR_BAD_STATE);
        else
            handle_lobby_msg(c, f);
        return;
    case MSG_LEAVE_ROOM:
        /* 나간 사람에게 따로 답하는 메시지는 없다: 보낸 쪽은 곧바로 로비로 돌아간다. */
        if (st == SESSION_LOBBY)
            reply_error(c, ERR_BAD_STATE);
        else
            room_leave(c);
        return;
    case MSG_REMATCH:
        if (st != SESSION_WAITING)
            reply_error(c, ERR_BAD_STATE);
        else
            reply_error(c, match_rematch(c->room, c));
        return;
    case MSG_GAME_IN:
        /* 대기 중에는 경기 끝과 입력이 엇갈려 도착할 수 있어 조용히 버린다(5.1절).
         * 로비에서 보낸 입력은 방이 없으니 상태 오류로 답한다. */
        if (st == SESSION_PLAYING)
            match_input(c->room, c, f->payload, f->len);
        else if (st == SESSION_LOBBY)
            reply_error(c, ERR_BAD_STATE);
        return;
    default:
        /* frame_check_header가 정의된 type만 넘기므로 오지 않는다. */
        conn_kill(c, "unexpected type");
        return;
    }
}
