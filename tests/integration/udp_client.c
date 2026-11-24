/* udp_client.c - 통합 테스트 클라이언트의 UDP 쪽(protocol.md 7장). 서버 코드와 독립적으로 바이트를 만든다. */
#include "client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "wire.h"

#define BIND_RETRY_MS 200 /* 7장 1번: 0.2초마다 다시 보낸다 */
#define BIND_GIVE_UP_MS 2000
#define DGRAM_MAX (4 + FRAME_HEADER_LEN + FRAME_MAX_PAYLOAD)

int udp_socket_open(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd == -1)
        return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(g_itest_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    /* connect한 UDP 소켓은 서버 주소에서 온 데이터그램만 받고, 출처 포트가 하나로 고정된다. */
    if (connect(fd, (struct sockaddr *)&a, sizeof a) == -1) {
        close(fd);
        return -1;
    }
    return fd;
}

void udp_send_bytes(int fd, const void *bytes, size_t n)
{
    /* 루프백이라 보내기가 실패할 일은 거의 없다. 실패하면 서버가 못 받은 것으로 드러난다. */
    ssize_t r;
    do
        r = send(fd, bytes, n, MSG_NOSIGNAL);
    while (r == -1 && errno == EINTR);
}

void udp_send_frame_tok(int fd, uint32_t token, uint8_t type, const void *payload, size_t len)
{
    uint8_t d[DGRAM_MAX];
    wire_put_u32(d, token);
    d[4] = PROTO_VERSION;
    d[5] = type;
    wire_put_u16(d + 6, (uint16_t)len);
    if (len > 0)
        memcpy(d + 4 + FRAME_HEADER_LEN, payload, len);
    udp_send_bytes(fd, d, 4 + FRAME_HEADER_LEN + len);
}

int udp_recv(int fd, msg *m, int timeout_ms)
{
    uint64_t deadline = itest_now_ms() + (uint64_t)timeout_ms;
    for (;;) {
        uint64_t now = itest_now_ms();
        int wait = now >= deadline ? 0 : (int)(deadline - now);
        struct pollfd p = {.fd = fd, .events = POLLIN};
        int r = poll(&p, 1, wait);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return 0;
        uint8_t d[DGRAM_MAX + 1];
        ssize_t n = recv(fd, d, sizeof d, MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (n < 0) /* 앞서 보낸 데이터그램의 ICMP 오류(포트 닫힘 등): 받은 것이 없다 */
            continue;
        /* 7장: 서버 → 클라이언트 데이터그램은 토큰 없이 프레임 정확히 하나 */
        if (n < FRAME_HEADER_LEN || d[0] != PROTO_VERSION ||
            (size_t)n != (size_t)FRAME_HEADER_LEN + wire_get_u16(d + 2))
            return -1;
        m->type = d[1];
        m->len = wire_get_u16(d + 2);
        memcpy(m->p, d + FRAME_HEADER_LEN, m->len);
        return 1;
    }
}

int client_udp_bind(client *c)
{
    if (c->ufd == -1 && (c->ufd = udp_socket_open()) == -1)
        return 0;
    uint64_t give_up = itest_now_ms() + BIND_GIVE_UP_MS;
    while (itest_now_ms() < give_up) {
        udp_send_frame_tok(c->ufd, c->udp_token, MSG_UDP_BIND, NULL, 0);
        uint64_t retry = itest_now_ms() + BIND_RETRY_MS;
        msg m;
        for (uint64_t now = itest_now_ms(); now < retry; now = itest_now_ms()) {
            int r = udp_recv(c->ufd, &m, (int)(retry - now));
            if (r == 1 && m.type == MSG_UDP_BOUND && m.len == 0)
                return 1;
            if (r == 0)
                break;
        }
    }
    return 0;
}

void send_pong_batch_udp(client *c, uint8_t axis)
{
    uint32_t last = ++c->seq;
    uint32_t n = last < 4 ? last : 4;
    uint8_t p[1 + 5 * 4];
    p[0] = (uint8_t)n;
    for (uint32_t i = 0; i < n; i++) {
        wire_put_u32(p + 1 + 5 * i, last - (n - 1) + i);
        p[1 + 5 * i + 4] = axis;
    }
    udp_send_frame_tok(c->ufd, c->udp_token, MSG_GAME_IN, p, 1 + 5 * n);
}
