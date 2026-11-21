/* frame.c - 프레임 파서와 헤더 검사. 설명은 frame.h. */
#include "frame.h"

#include <string.h>

#include "game_api.h"
#include "wire.h"

/* CREATE_ROOM 고정 부분: game_id(2) + visibility(1) + settings_len(1). */
#define CREATE_ROOM_FIXED 4

void frame_reader_init(frame_reader *r)
{
    r->rpos = 0;
    r->wpos = 0;
    r->err = NULL;
}

/* type별 허용 페이로드 길이(protocol.md 4.1 표). 모르는 type이면 0을 돌려준다.
 * 이 표에 없는 번호(0x00, 정의 안 된 번호, 0x80 이상)는 모두 즉시 종료 대상이다. */
static int payload_limits(uint8_t type, uint16_t *min, uint16_t *max)
{
    switch (type) {
    case MSG_LIST_ROOMS:
    case MSG_QUICK_JOIN: *min = *max = 2; return 1;
    case MSG_CREATE_ROOM:
        *min = CREATE_ROOM_FIXED;
        *max = CREATE_ROOM_FIXED + GAME_MAX_SETTINGS;
        return 1;
    case MSG_JOIN_ROOM:
    case MSG_PING: *min = *max = 4; return 1;
    case MSG_JOIN_BY_CODE: *min = *max = ROOM_CODE_LEN; return 1;
    case MSG_LEAVE_ROOM:
    case MSG_REMATCH: *min = *max = 0; return 1;
    case MSG_GAME_IN:
        /* 게임별 상한(max_input_len)은 방을 알아야 하므로 session에서 다시 본다. */
        *min = 1;
        *max = GAME_MAX_PAYLOAD;
        return 1;
    default: return 0;
    }
}

const char *frame_check_header(const uint8_t header[FRAME_HEADER_LEN])
{
    if (header[0] != PROTO_VERSION)
        return "bad version";
    uint16_t len = wire_get_u16(header + 2);
    if (len > FRAME_MAX_PAYLOAD)
        return "len over 512";
    uint16_t min, max;
    if (!payload_limits(header[1], &min, &max))
        return "undefined or server-to-client type";
    if (len < min || len > max)
        return "bad length for type";
    return NULL;
}

const char *frame_check_payload(uint8_t type, const uint8_t *payload, uint16_t len)
{
    if (type == MSG_CREATE_ROOM && len != CREATE_ROOM_FIXED + payload[3])
        return "create_room length does not match settings_len";
    return NULL;
}

void frame_put_header(uint8_t out[FRAME_HEADER_LEN], uint8_t type, uint16_t len)
{
    out[0] = PROTO_VERSION;
    out[1] = type;
    wire_put_u16(out + 2, len);
}

uint8_t *frame_reader_space(frame_reader *r, size_t *avail)
{
    /* 꺼낸 프레임 자리를 매번 당기면 memmove가 프레임마다 생긴다. 새로 받기 직전에만 당긴다.
     * 남는 조각은 최대 프레임보다 작으므로 비용은 516바이트 이하다. */
    if (r->rpos > 0) {
        size_t left = r->wpos - r->rpos;
        memmove(r->buf, r->buf + r->rpos, left);
        r->rpos = 0;
        r->wpos = left;
    }
    *avail = FRAME_RBUF_CAP - r->wpos;
    return r->buf + r->wpos;
}

void frame_reader_commit(frame_reader *r, size_t n)
{
    r->wpos += n;
}

size_t frame_reader_feed(frame_reader *r, const uint8_t *data, size_t n)
{
    size_t avail;
    uint8_t *dst = frame_reader_space(r, &avail);
    if (n > avail)
        n = avail;
    memcpy(dst, data, n);
    frame_reader_commit(r, n);
    return n;
}

frame_status frame_reader_next(frame_reader *r, frame *out)
{
    if (r->err != NULL)
        return FRAME_BAD;
    size_t have = r->wpos - r->rpos;
    if (have < FRAME_HEADER_LEN)
        return FRAME_NEED_MORE;
    const uint8_t *h = r->buf + r->rpos;
    r->err = frame_check_header(h);
    if (r->err != NULL)
        return FRAME_BAD;
    uint16_t len = wire_get_u16(h + 2);
    if (have < (size_t)FRAME_HEADER_LEN + len)
        return FRAME_NEED_MORE;
    r->err = frame_check_payload(h[1], h + FRAME_HEADER_LEN, len);
    if (r->err != NULL)
        return FRAME_BAD;
    out->type = h[1];
    out->len = len;
    out->payload = h + FRAME_HEADER_LEN;
    r->rpos += FRAME_HEADER_LEN + len;
    return FRAME_OK;
}
