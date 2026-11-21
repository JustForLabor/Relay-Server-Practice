/* frame.h - 누적 버퍼에서 완성된 프레임만 꺼내는 파서 (protocol.md 3장, 5.4절 마지막 문단).
 *
 * TCP는 바이트 스트림이라 recv 한 번에 프레임 반 개나 세 개 반이 올 수 있다. 받은 바이트를
 * 쌓아 두고 "헤더가 다 왔나 → 헤더가 말한 길이만큼 다 왔나"를 본 뒤에만 프레임을 넘긴다.
 * 헤더 검사(버전 · type · 길이)는 헤더 4바이트가 오자마자 한다: 512를 넘는 길이를 보고
 * 페이로드를 기다리면 상대가 버퍼를 붙잡아 둘 수 있기 때문이다.
 */
#ifndef FRAME_H
#define FRAME_H

#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

/* 연결당 수신 버퍼(overview.md 6.4). 가장 큰 프레임(4 + 512)보다 넉넉히 커서, 완성된 프레임을
 * 다 꺼낸 뒤 남는 조각(516 미만)을 앞으로 당기면 항상 recv할 자리가 생긴다. */
#define FRAME_RBUF_CAP 2048

typedef struct frame {
    uint8_t type;
    uint16_t len;
    const uint8_t *payload; /* 다음 frame_reader_space · feed 호출 전까지만 유효하다 */
} frame;

typedef enum {
    FRAME_OK = 0,
    FRAME_NEED_MORE = 1,
    FRAME_BAD = 2, /* 연결을 ERROR 없이 끊어야 하는 위반. 한 번 나면 계속 BAD */
} frame_status;

typedef struct frame_reader {
    uint8_t buf[FRAME_RBUF_CAP];
    size_t rpos;     /* 아직 꺼내지 않은 첫 바이트 */
    size_t wpos;     /* 받은 바이트의 끝 */
    const char *err; /* FRAME_BAD일 때 이유(로그용) */
} frame_reader;

void frame_reader_init(frame_reader *r);

/* recv로 바로 채울 빈자리. 앞쪽 소비한 바이트를 당겨 자리를 만든 뒤 돌려준다. */
uint8_t *frame_reader_space(frame_reader *r, size_t *avail);
void frame_reader_commit(frame_reader *r, size_t n);

/* data를 들어갈 만큼 복사한다(단위 테스트 · 조각 입력용). 복사한 바이트 수를 돌려준다. */
size_t frame_reader_feed(frame_reader *r, const uint8_t *data, size_t n);

/* 완성된 프레임 하나를 꺼낸다. */
frame_status frame_reader_next(frame_reader *r, frame *out);

/* 헤더 4바이트만으로 하는 검사. 통과하면 NULL, 아니면 이유 문자열.
 * 버전, 정의된 클라이언트 → 서버 type인지, len 512 이하, 그 type의 허용 길이 범위. */
const char *frame_check_header(const uint8_t header[FRAME_HEADER_LEN]);

/* 페이로드가 있어야 알 수 있는 길이 검사(CREATE_ROOM의 4 + settings_len). */
const char *frame_check_payload(uint8_t type, const uint8_t *payload, uint16_t len);

/* 헤더 4바이트를 쓴다. */
void frame_put_header(uint8_t out[FRAME_HEADER_LEN], uint8_t type, uint16_t len);

#endif /* FRAME_H */
