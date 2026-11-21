/* test_frame.c - 프레임 파서 단위 테스트 (protocol.md 3장, 5.4절).
 *
 * 같은 바이트 열을 1바이트씩, 무작위 크기 조각으로 넣어도 같은 프레임이 같은 순서로 나와야 한다.
 * TCP는 경계를 지켜 주지 않으므로 파서가 조각 크기에 따라 달라지면 실제 네트워크에서 깨진다.
 * 거부 케이스는 헤더만 보고(페이로드를 기다리지 않고) 끊어야 하는 것까지 확인한다.
 */
#include <stdlib.h>
#include <string.h>

#include "frame.h"
#include "game_api.h"
#include "unit_check.h"
#include "wire.h"

#define MAX_FRAMES 64
#define STREAM_CAP (MAX_FRAMES * (FRAME_HEADER_LEN + FRAME_MAX_PAYLOAD))

typedef struct expected {
    uint8_t type;
    uint16_t len;
    size_t offset; /* 스트림 안에서 페이로드 시작 */
} expected;

static uint8_t g_stream[STREAM_CAP];
static size_t g_stream_len;
static expected g_exp[MAX_FRAMES];
static int g_exp_count;

static void add_frame(uint8_t type, uint16_t len, const uint8_t *payload)
{
    uint8_t *p = g_stream + g_stream_len;
    frame_put_header(p, type, len);
    memcpy(p + FRAME_HEADER_LEN, payload, len);
    g_exp[g_exp_count].type = type;
    g_exp[g_exp_count].len = len;
    g_exp[g_exp_count].offset = g_stream_len + FRAME_HEADER_LEN;
    g_exp_count++;
    g_stream_len += FRAME_HEADER_LEN + len;
}

/* 모든 클라이언트 → 서버 type을 허용 길이 경계값으로 하나 이상 넣는다. */
static void build_stream(void)
{
    uint8_t p[GAME_MAX_PAYLOAD];
    for (int i = 0; i < GAME_MAX_PAYLOAD; i++)
        p[i] = (uint8_t)(i * 7 + 3);
    add_frame(0x10, 2, p);
    uint8_t create[4 + GAME_MAX_SETTINGS];
    memcpy(create, p, sizeof create);
    create[3] = 25;
    add_frame(0x11, 4 + 25, create);
    create[3] = 0;
    add_frame(0x11, 4, create);
    create[3] = GAME_MAX_SETTINGS;
    add_frame(0x11, 4 + GAME_MAX_SETTINGS, create);
    add_frame(0x12, 2, p);
    add_frame(0x13, 4, p);
    add_frame(0x14, 6, p);
    add_frame(0x15, 0, p);
    add_frame(0x16, 4, p);
    add_frame(0x17, 0, p);
    add_frame(0x40, 1, p);
    add_frame(0x40, 5, p);
    add_frame(0x40, GAME_MAX_PAYLOAD, p);
    add_frame(0x40, GAME_MAX_PAYLOAD, p); /* 최대 프레임 둘이 이어져도 수신 버퍼에 들어간다 */
    add_frame(0x16, 4, p + 100);
}

/* 꺼낸 프레임이 기대한 순서 · 내용과 같은지 보고, 다음에 기대할 번호를 돌려준다. */
static int drain(frame_reader *r, int next)
{
    frame f;
    frame_status st;
    while ((st = frame_reader_next(r, &f)) == FRAME_OK) {
        CHECK(next < g_exp_count);
        if (next >= g_exp_count)
            return next;
        CHECK_EQ(f.type, g_exp[next].type);
        CHECK_EQ(f.len, g_exp[next].len);
        CHECK(memcmp(f.payload, g_stream + g_exp[next].offset, f.len) == 0);
        next++;
    }
    CHECK_EQ(st, FRAME_NEED_MORE);
    return next;
}

static void feed_in_chunks(size_t (*chunk)(void))
{
    frame_reader r;
    frame_reader_init(&r);
    int next = 0;
    size_t pos = 0;
    while (pos < g_stream_len) {
        size_t want = chunk();
        if (want > g_stream_len - pos)
            want = g_stream_len - pos;
        /* 수신 버퍼가 차면 feed가 일부만 받는다. 서버처럼 꺼낸 뒤 다시 넣는다. */
        while (want > 0) {
            size_t took = frame_reader_feed(&r, g_stream + pos, want);
            pos += took;
            want -= took;
            next = drain(&r, next);
        }
    }
    CHECK_EQ(next, g_exp_count);
    CHECK_EQ(r.wpos - r.rpos, 0);
}

static size_t one_byte(void)
{
    return 1;
}

static size_t random_size(void)
{
    return 1 + (size_t)(rand() % 1200);
}

/* bytes를 통째로 넣었을 때 BAD가 나와야 한다. */
static void expect_bad(const uint8_t *bytes, size_t n)
{
    frame_reader r;
    frame_reader_init(&r);
    frame_reader_feed(&r, bytes, n);
    frame f;
    CHECK_EQ(frame_reader_next(&r, &f), FRAME_BAD);
    CHECK(r.err != NULL);
    /* 한 번 BAD면 뒤에 정상 프레임이 와도 계속 BAD다(연결을 끊을 것이므로). */
    uint8_t ping[8] = {1, 0x16, 0, 4, 0, 0, 0, 1};
    frame_reader_feed(&r, ping, sizeof ping);
    CHECK_EQ(frame_reader_next(&r, &f), FRAME_BAD);
}

static void test_rejects(void)
{
    uint8_t zero[FRAME_MAX_PAYLOAD + 8];
    memset(zero, 0, sizeof zero);

    uint8_t bad_version[8] = {2, 0x16, 0, 4, 0, 0, 0, 0};
    expect_bad(bad_version, sizeof bad_version);
    uint8_t version0[8] = {0, 0x16, 0, 4, 0, 0, 0, 0};
    expect_bad(version0, sizeof version0);

    /* 정의되지 않은 type, 0x00, 아직 쓰지 않는 게임 영역, 서버 → 클라이언트 type */
    const uint8_t bad_types[] = {0x00, 0x01, 0x0F, 0x18, 0x20, 0x3F, 0x41, 0x7F,
                                 0x80, 0x87, 0x8F, 0xC0, 0xFF};
    for (size_t i = 0; i < sizeof bad_types; i++) {
        uint8_t h[4] = {1, bad_types[i], 0, 0};
        expect_bad(h, sizeof h);
    }

    /* len 513: 헤더 4바이트만으로 거부해야 한다(페이로드를 기다리지 않는다). */
    uint8_t big[4] = {1, 0x40, 0x02, 0x01};
    expect_bad(big, sizeof big);
    uint8_t huge[4] = {1, 0x40, 0xFF, 0xFF};
    expect_bad(huge, sizeof huge);

    /* 고정 길이 메시지의 길이가 다름(헤더만으로 판단) */
    uint8_t wrong_len[][4] = {
        {1, 0x10, 0, 3}, {1, 0x10, 0, 1}, {1, 0x12, 0, 0}, {1, 0x13, 0, 5},
        {1, 0x14, 0, 5}, {1, 0x15, 0, 1}, {1, 0x16, 0, 0}, {1, 0x17, 0, 4},
        {1, 0x40, 0, 0}, {1, 0x40, 0x01, 0xF5}, /* GAME_IN 501 */
        {1, 0x11, 0, 3}, {1, 0x11, 0, 69},      /* CREATE_ROOM 4–68 밖 */
    };
    for (size_t i = 0; i < sizeof wrong_len / sizeof wrong_len[0]; i++)
        expect_bad(wrong_len[i], 4);

    /* CREATE_ROOM: len이 4 + settings_len과 다르면 페이로드가 온 뒤 거부한다. */
    uint8_t create[4 + 7] = {1, 0x11, 0, 7, 0, 1, 0, 2, 9, 9, 9};
    expect_bad(create, sizeof create);
    uint8_t create_short[4 + 6] = {1, 0x11, 0, 6, 0, 1, 0, 3, 9, 9};
    expect_bad(create_short, sizeof create_short);

    /* 헤더가 다 오기 전에는 판단하지 않는다 */
    frame_reader r;
    frame_reader_init(&r);
    frame f;
    frame_reader_feed(&r, big, 3);
    CHECK_EQ(frame_reader_next(&r, &f), FRAME_NEED_MORE);
    frame_reader_feed(&r, big + 3, 1);
    CHECK_EQ(frame_reader_next(&r, &f), FRAME_BAD);
}

static void test_header_check_direct(void)
{
    uint8_t ok[4] = {1, 0x40, 0x01, 0xF4}; /* GAME_IN 500 */
    CHECK(frame_check_header(ok) == NULL);
    uint8_t h[4];
    frame_put_header(h, 0x85, 0x1234);
    CHECK_EQ(h[0], 1);
    CHECK_EQ(h[1], 0x85);
    CHECK_EQ(wire_get_u16(h + 2), 0x1234);
}

static void test_wire(void)
{
    uint8_t b[4];
    wire_put_u32(b, 0x0A0B0C0Du);
    CHECK(b[0] == 0x0A && b[1] == 0x0B && b[2] == 0x0C && b[3] == 0x0D);
    CHECK_EQ(wire_get_u32(b), 0x0A0B0C0D);
    wire_put_u16(b, 0xBEEF);
    CHECK(b[0] == 0xBE && b[1] == 0xEF);
    CHECK_EQ(wire_get_u16(b), 0xBEEF);
}

int main(void)
{
    build_stream();
    feed_in_chunks(one_byte);
    srand(12345);
    for (int i = 0; i < 300; i++)
        feed_in_chunks(random_size);
    test_rejects();
    test_header_check_direct();
    test_wire();
    return UNIT_DONE("test_frame");
}
