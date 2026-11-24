/* udp.c - UDP 묶기 · 보내기 · 받기. 설명은 udp.h. */
#include "udp.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>

#include "frame.h"
#include "session.h"
#include "stats.h"
#include "wire.h"

/* 한 번 깨어날 때 읽는 데이터그램 수 상한. 쉬지 않고 쏟아지는 UDP가 TCP 연결과 진행을 굶기지
 * 않게 한다. LT라 남은 것은 다음 바퀴에 다시 알려 준다. */
#define UDP_READS_PER_WAKE 64
/* 토큰 + 가장 큰 프레임. 1바이트 더 받아 보면 넘친 데이터그램을 가려낼 수 있다(MSG_TRUNC). */
#define UDP_MAX_DGRAM (UDP_TOKEN_LEN + FRAME_HEADER_LEN + FRAME_MAX_PAYLOAD)

static int g_fd = -1;

void udp_set_socket(int fd)
{
    g_fd = fd;
}

static void send_to(const struct sockaddr_in *to, const uint8_t *buf, size_t n)
{
    /* 논블로킹 소켓이라 커널 버퍼가 차면 EAGAIN. 비신뢰 메시지는 다시 보내지 않고 버린다
     * (protocol.md 7장 "서버는 사라진 상태를 다시 보내지 않는다"). */
    ssize_t r;
    do
        r = sendto(g_fd, buf, n, MSG_DONTWAIT | MSG_NOSIGNAL, (const struct sockaddr *)to,
                   sizeof *to);
    while (r == -1 && errno == EINTR);
    if (r == (ssize_t)n) {
        g_stats.udp_tx_datagrams++;
        g_stats.udp_tx_bytes += n;
    } else {
        g_stats.udp_tx_dropped++;
    }
}

bool udp_send_frame(conn *c, uint8_t type, const uint8_t *payload, size_t len)
{
    if (g_fd == -1 || !c->udp_bound)
        return false;
    if (c->closing) /* TCP 쪽과 같이, 끊기로 한 연결에는 더 보내지 않는다 */
        return true;
    uint8_t buf[FRAME_HEADER_LEN + FRAME_MAX_PAYLOAD];
    frame_put_header(buf, type, (uint16_t)len);
    if (len > 0)
        memcpy(buf + FRAME_HEADER_LEN, payload, len);
    send_to(&c->udp_addr, buf, FRAME_HEADER_LEN + len);
    return true;
}

static bool same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

/* 데이터그램 하나 = u32 udp_token + 프레임 하나(protocol.md 7장). 규칙에 어긋나면 조용히 버린다. */
static void handle_datagram(const uint8_t *d, size_t n, const struct sockaddr_in *from)
{
    if (n < UDP_TOKEN_LEN + FRAME_HEADER_LEN || n > UDP_MAX_DGRAM)
        return;
    const uint8_t *h = d + UDP_TOKEN_LEN;
    uint16_t len = wire_get_u16(h + 2);
    if (h[0] != PROTO_VERSION || n != (size_t)UDP_TOKEN_LEN + FRAME_HEADER_LEN + len)
        return; /* 데이터그램 하나에 프레임 정확히 하나 */
    conn *c = conn_find_by_token(wire_get_u32(d));
    if (c == NULL || c->closing)
        return;
    switch (h[1]) {
    case MSG_UDP_BIND:
        if (len != 0)
            return;
        /* 7장 2번: 이미 묶였어도 새 출처로 바꾼다(공유기가 포트를 바꾸는 경우). 답은 요청한 주소로. */
        conn_udp_bind(c, from);
        uint8_t bound[FRAME_HEADER_LEN];
        frame_put_header(bound, MSG_UDP_BOUND, 0);
        send_to(from, bound, sizeof bound);
        return;
    case MSG_GAME_IN:
        /* 길이 범위(1–500)는 TCP와 같은 헤더 검사로 본다. 묶인 주소가 아닌 곳에서 온 입력은
         * 토큰을 엿본 제삼자일 수 있어 받지 않는다(7장 4번). */
        if (frame_check_header(h) != NULL || !c->udp_bound || !same_addr(&c->udp_addr, from))
            return;
        session_udp_game_in(c, h + FRAME_HEADER_LEN, len);
        return;
    default:
        return; /* 로비 · 방 메시지는 TCP로만 받는다 */
    }
}

void udp_on_readable(void)
{
    if (g_fd == -1)
        return;
    for (int i = 0; i < UDP_READS_PER_WAKE; i++) {
        uint8_t buf[UDP_MAX_DGRAM];
        struct sockaddr_in from;
        socklen_t flen = sizeof from;
        /* MSG_TRUNC: 버퍼보다 큰 데이터그램이면 잘린 길이 대신 실제 길이를 돌려받아 버린다. */
        ssize_t n = recvfrom(g_fd, buf, sizeof buf, MSG_TRUNC, (struct sockaddr *)&from, &flen);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            /* EAGAIN이면 다 읽었다. 다른 오류(앞서 보낸 데이터그램의 ICMP 오류 등)는 한 번 보고되고
             * 사라지므로 다음 바퀴에 다시 읽는다. */
            return;
        }
        g_stats.udp_rx_datagrams++;
        g_stats.udp_rx_bytes += (uint64_t)n;
        if (flen != sizeof from || from.sin_family != AF_INET)
            continue;
        handle_datagram(buf, (size_t)n, &from);
    }
}
