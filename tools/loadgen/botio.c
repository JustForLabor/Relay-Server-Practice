/* botio.c - 봇의 소켓 입출력: 접속, 프레임 자르기, 송신 버퍼, UDP 데이터그램.
 *
 * 받은 메시지의 뜻은 bot.c의 bot_on_msg가 맡는다. 여기서는 바이트 경계만 다룬다. */
#include <errno.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "lg.h"
#include "proto.h"

void bot_close(bot *b)
{
    /* close가 epoll 등록도 지운다(이 fd를 다른 곳에서 복제하지 않으므로). */
    if (b->fd >= 0)
        close(b->fd);
    if (b->ufd >= 0)
        close(b->ufd);
    b->fd = b->ufd = -1;
    b->state = B_DEAD;
    b->connect_at = b->bind_at = b->input_at = b->ping_at = 0;
}

void bot_fail(bot *b, int reason)
{
    if (b->state == B_DEAD)
        return;
    if (b->welcomed) {
        g.st.dropped++;
        g.st.drop[reason]++;
    } else {
        g.st.conn_fail++;
        g.st.fail[reason]++;
    }
    bot_close(b);
}

static void set_events(bot *b, int want_out)
{
    if (b->want_out == want_out)
        return;
    struct epoll_event ev = {.events = EPOLLIN | (want_out ? EPOLLOUT : 0),
                             .data.u64 = (uint64_t)b->idx << 1};
    epoll_ctl(g.epfd, EPOLL_CTL_MOD, b->fd, &ev);
    b->want_out = (uint8_t)want_out;
}

static void flush_out(bot *b)
{
    size_t off = 0;
    while (off < b->wlen) {
        ssize_t n = send(b->fd, b->wbuf + off, b->wlen - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else {
            bot_fail(b, F_CLOSED);
            return;
        }
    }
    memmove(b->wbuf, b->wbuf + off, b->wlen - off);
    b->wlen -= off;
    set_events(b, b->wlen > 0);
}

void bot_send_frame(bot *b, uint8_t type, const uint8_t *payload, uint16_t len)
{
    if (b->wlen + P_HDR + len > WBUF_CAP) {
        bot_fail(b, F_TX_OVERFLOW);
        return;
    }
    put_hdr(b->wbuf + b->wlen, type, len);
    if (len > 0)
        memcpy(b->wbuf + b->wlen + P_HDR, payload, len);
    b->wlen += P_HDR + len;
    flush_out(b);
}

/* 클라이언트 → 서버 데이터그램 = u32 udp_token + 프레임 하나 (7장) */
void bot_send_udp(bot *b, uint8_t type, const uint8_t *payload, uint16_t len)
{
    uint8_t buf[4 + P_HDR + 32];
    put_u32(buf, b->udp_token);
    put_hdr(buf + 4, type, len);
    if (len > 0)
        memcpy(buf + 4 + P_HDR, payload, len);
    if (send(b->ufd, buf, 4 + P_HDR + (size_t)len, MSG_DONTWAIT) < 0)
        g.st.udp_tx_err++;
}

void bot_connect(bot *b)
{
    b->connect_at = 0;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        g.st.conn_fail++;
        g.st.fail[F_SOCKET]++;
        b->state = B_DEAD;
        return;
    }
    b->fd = fd;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (connect(fd, (const struct sockaddr *)&g.cfg.addr, sizeof g.cfg.addr) < 0 &&
        errno != EINPROGRESS) {
        bot_fail(b, F_CONNECT);
        return;
    }
    b->state = B_CONNECTING;
    b->want_out = 1;
    struct epoll_event ev = {.events = EPOLLIN | EPOLLOUT, .data.u64 = (uint64_t)b->idx << 1};
    if (epoll_ctl(g.epfd, EPOLL_CTL_ADD, fd, &ev) < 0)
        bot_fail(b, F_SOCKET);
}

int bot_open_udp(bot *b)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    /* connect한 UDP 소켓은 서버 주소에서 온 데이터그램만 받는다 */
    if (fd < 0 || connect(fd, (const struct sockaddr *)&g.cfg.addr, sizeof g.cfg.addr) < 0) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    struct epoll_event ev = {.events = EPOLLIN, .data.u64 = ((uint64_t)b->idx << 1) | 1};
    if (epoll_ctl(g.epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        close(fd);
        return -1;
    }
    b->ufd = fd;
    return 0;
}

void bot_close_udp(bot *b)
{
    if (b->ufd >= 0)
        close(b->ufd);
    b->ufd = -1;
}

/* 쌓인 바이트에서 완성된 프레임을 모두 처리한다. 한 번 깨어날 때 recv는 8번까지:
 * 한 봇이 루프를 붙잡아 다른 봇의 입력 시각이 밀리지 않게 한다(남은 것은 LT가 다시 알림). */
static void read_tcp(bot *b)
{
    for (int reads = 0; reads < 8; reads++) {
        ssize_t n = recv(b->fd, b->rbuf + b->rlen, RBUF_CAP - b->rlen, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (n <= 0) {
            bot_fail(b, F_CLOSED);
            return;
        }
        b->rlen += (size_t)n;
        size_t off = 0;
        while (b->rlen - off >= P_HDR) {
            const uint8_t *h = b->rbuf + off;
            uint16_t len = get_u16(h + 2);
            if (h[0] != P_VERSION || len > P_MAX_PAYLOAD) {
                bot_fail(b, F_PROTOCOL);
                return;
            }
            if (b->rlen - off < P_HDR + (size_t)len)
                break;
            bot_on_msg(b, h[1], h + P_HDR, len, 0);
            if (b->state == B_DEAD)
                return;
            off += P_HDR + len;
        }
        memmove(b->rbuf, b->rbuf + off, b->rlen - off);
        b->rlen -= off;
    }
}

void bot_on_tcp(bot *b, uint32_t events)
{
    if (b->state == B_CONNECTING) {
        int err = 0;
        socklen_t el = sizeof err;
        if (getsockopt(b->fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err != 0) {
            bot_fail(b, F_CONNECT);
            return;
        }
        b->state = B_WELCOME;
        set_events(b, 0);
    }
    if ((events & EPOLLOUT) && b->wlen > 0)
        flush_out(b);
    if (b->state != B_DEAD && (events & (EPOLLIN | EPOLLHUP | EPOLLERR)))
        read_tcp(b);
}

void bot_on_udp(bot *b)
{
    uint8_t buf[P_HDR + P_MAX_PAYLOAD + 1];
    for (int i = 0; i < 16 && b->ufd >= 0; i++) {
        ssize_t n = recv(b->ufd, buf, sizeof buf, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR || errno == ECONNREFUSED)
                continue; /* ECONNREFUSED: 앞서 보낸 데이터그램의 ICMP 거부. 소켓은 그대로 쓴다 */
            return;
        }
        /* 7장: 어긋난 데이터그램은 조용히 버린다 */
        if (n < P_HDR || buf[0] != P_VERSION || (size_t)n != P_HDR + (size_t)get_u16(buf + 2))
            continue;
        bot_on_msg(b, buf[1], buf + P_HDR, get_u16(buf + 2), 1);
        if (b->state == B_DEAD)
            return;
    }
}
