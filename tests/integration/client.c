/* client.c - 통합 테스트 클라이언트 구현. */
#include "client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "wire.h"

int g_itest_failures;
uint16_t g_itest_port;

void itest_fail(const char *file, int line, const char *what)
{
    g_itest_failures++;
    fprintf(stderr, "FAIL %s:%d: %s\n", file, line, what);
}

uint64_t itest_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

int client_connect(client *c, int rcvbuf)
{
    memset(c, 0, sizeof *c);
    c->fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (c->fd == -1)
        return -1;
    if (rcvbuf > 0) /* 창 크기 협상에 반영되도록 connect 전에 줄인다 */
        setsockopt(c->fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(g_itest_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(c->fd, (struct sockaddr *)&a, sizeof a) == -1) {
        close(c->fd);
        c->fd = -1;
        return -1;
    }
    int one = 1;
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (client_recv(c, &c->welcome, T_LONG) != 1 || c->welcome.type != MSG_WELCOME ||
        c->welcome.len < 9)
        return -1;
    c->conn_id = wire_get_u32(c->welcome.p);
    c->udp_token = wire_get_u32(c->welcome.p + 4);
    return 0;
}

void client_close(client *c)
{
    if (c->fd != -1)
        close(c->fd);
    c->fd = -1;
}

void client_send_raw(client *c, const void *bytes, size_t n)
{
    const uint8_t *p = bytes;
    while (n > 0) {
        ssize_t k = send(c->fd, p, n, MSG_NOSIGNAL);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0) /* 서버가 이미 끊은 연결에 보내는 경우(견고성 시나리오)는 그냥 멈춘다 */
            return;
        p += k;
        n -= (size_t)k;
    }
}

void client_send(client *c, uint8_t type, const void *payload, size_t len)
{
    uint8_t f[FRAME_HEADER_LEN + FRAME_MAX_PAYLOAD];
    f[0] = PROTO_VERSION;
    f[1] = type;
    wire_put_u16(f + 2, (uint16_t)len);
    if (len > 0)
        memcpy(f + FRAME_HEADER_LEN, payload, len);
    client_send_raw(c, f, FRAME_HEADER_LEN + len);
}

/* 버퍼에 완성된 프레임이 있으면 꺼낸다. 1 꺼냄, 0 모자람, -1 형식 오류. */
static int take(client *c, msg *m)
{
    if (c->len < FRAME_HEADER_LEN)
        return 0;
    uint16_t len = wire_get_u16(c->buf + 2);
    if (c->buf[0] != PROTO_VERSION || len > FRAME_MAX_PAYLOAD) {
        fprintf(stderr, "client: server sent malformed header\n");
        return -1;
    }
    if (c->len < (size_t)FRAME_HEADER_LEN + len)
        return 0;
    m->type = c->buf[1];
    m->len = len;
    memcpy(m->p, c->buf + FRAME_HEADER_LEN, len);
    size_t used = FRAME_HEADER_LEN + len;
    memmove(c->buf, c->buf + used, c->len - used);
    c->len -= used;
    return 1;
}

int client_recv(client *c, msg *m, int timeout_ms)
{
    uint64_t deadline = itest_now_ms() + (uint64_t)timeout_ms;
    for (;;) {
        int t = take(c, m);
        if (t != 0)
            return t;
        /* 시간이 다 됐어도 poll(0)으로 한 번은 소켓을 본다: timeout 0은 "있는 것만 읽기"다. */
        uint64_t now = itest_now_ms();
        int wait = now >= deadline ? 0 : (int)(deadline - now);
        struct pollfd pfd = {.fd = c->fd, .events = POLLIN};
        int r = poll(&pfd, 1, wait);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return r == 0 ? 0 : -1;
        ssize_t n = recv(c->fd, c->buf + c->len, sizeof c->buf - c->len, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        c->len += (size_t)n;
    }
}

int client_wait_for(client *c, uint8_t type, msg *m, int timeout_ms)
{
    uint64_t deadline = itest_now_ms() + (uint64_t)timeout_ms;
    for (;;) {
        uint64_t now = itest_now_ms();
        int left = now >= deadline ? 0 : (int)(deadline - now);
        int r = client_recv(c, m, left);
        if (r != 1) {
            fprintf(stderr, "client %u: waiting for 0x%02X: %s\n", c->conn_id, type,
                    r == 0 ? "timeout" : "closed");
            return 0;
        }
        if (m->type == type)
            return 1;
        if (m->type == MSG_GAME_OUT)
            continue;
        fprintf(stderr, "client %u: waiting for 0x%02X, got 0x%02X\n", c->conn_id, type,
                m->type);
        return 0;
    }
}

int client_expect_u8(client *c, uint8_t type, uint8_t value)
{
    msg m;
    if (!client_wait_for(c, type, &m, T_LONG))
        return 0;
    if (m.len != 1 || m.p[0] != value) {
        fprintf(stderr, "client %u: 0x%02X carried %u, expected %u\n", c->conn_id, type,
                m.len ? m.p[0] : 999u, value);
        return 0;
    }
    return 1;
}

int client_expect_error(client *c, uint16_t code)
{
    msg m;
    if (!client_wait_for(c, MSG_ERROR, &m, T_LONG))
        return 0;
    if (m.len < 2 || wire_get_u16(m.p) != code) {
        fprintf(stderr, "client %u: ERROR code %u, expected %u\n", c->conn_id,
                m.len >= 2 ? wire_get_u16(m.p) : 0u, code);
        return 0;
    }
    return 1;
}

int client_wait_closed(client *c, int timeout_ms, int *frames)
{
    uint64_t deadline = itest_now_ms() + (uint64_t)timeout_ms;
    int count = 0;
    for (;;) {
        uint64_t now = itest_now_ms();
        if (now >= deadline)
            break;
        msg m;
        int r = client_recv(c, &m, (int)(deadline - now));
        if (r == -1) {
            if (frames != NULL)
                *frames = count;
            return 1;
        }
        if (r == 0)
            break;
        count++;
    }
    if (frames != NULL)
        *frames = count;
    return 0;
}

void client_quit(client *c)
{
    if (c->fd == -1)
        return;
    client_send(c, MSG_LEAVE_ROOM, NULL, 0);
    uint32_t mark = 0x51000000u + ++c->ping_seq;
    send_u32(c, MSG_PING, mark);
    msg m;
    uint64_t deadline = itest_now_ms() + T_LONG;
    for (;;) {
        uint64_t now = itest_now_ms();
        if (now >= deadline || client_recv(c, &m, (int)(deadline - now)) != 1) {
            itest_fail(__FILE__, __LINE__, "client_quit: no PONG");
            break;
        }
        if (m.type == MSG_PONG && m.len == 8 && wire_get_u32(m.p) == mark)
            break;
    }
    client_close(c);
}

void send_u16(client *c, uint8_t type, uint16_t v)
{
    uint8_t p[2];
    wire_put_u16(p, v);
    client_send(c, type, p, sizeof p);
}

void send_u32(client *c, uint8_t type, uint32_t v)
{
    uint8_t p[4];
    wire_put_u32(p, v);
    client_send(c, type, p, sizeof p);
}

void send_create(client *c, uint16_t game_id, uint8_t visibility, const uint8_t *settings,
                 uint8_t settings_len)
{
    uint8_t p[4 + 64];
    wire_put_u16(p, game_id);
    p[2] = visibility;
    p[3] = settings_len;
    if (settings_len > 0)
        memcpy(p + 4, settings, settings_len);
    client_send(c, MSG_CREATE_ROOM, p, 4u + settings_len);
}

void send_pong_input(client *c, uint8_t axis)
{
    uint8_t p[5];
    wire_put_u32(p, ++c->seq);
    p[4] = axis;
    client_send(c, MSG_GAME_IN, p, sizeof p);
}

int client_expect_joined(client *c, room_info *ri)
{
    msg m;
    if (!client_wait_for(c, MSG_ROOM_JOINED, &m, T_LONG))
        return 0;
    if (m.len < 17 || m.len != 17 + m.p[16]) {
        fprintf(stderr, "client %u: ROOM_JOINED bad length %u\n", c->conn_id, m.len);
        return 0;
    }
    ri->room_id = wire_get_u32(m.p);
    ri->game_id = wire_get_u16(m.p + 4);
    ri->slot = m.p[6];
    ri->max_players = m.p[7];
    ri->visibility = m.p[8];
    memcpy(ri->code, m.p + 9, ROOM_CODE_LEN);
    ri->mask = m.p[15];
    ri->settings_len = m.p[16];
    memcpy(ri->settings, m.p + 17, ri->settings_len);
    return 1;
}
