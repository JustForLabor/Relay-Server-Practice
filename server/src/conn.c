/* conn.c - 연결 테이블과 송수신 버퍼. 설명은 conn.h. */
#include "conn.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "rng.h"
#include "stats.h"

/* 송신 버퍼를 처음부터 64KB로 잡으면 접속 수만큼 메모리를 미리 쓴다. 작게 시작해 두 배씩 늘린다. */
#define OUT_INITIAL_CAP 1024u

static conn **g_table;      /* fd → 연결 */
static size_t g_table_size;
static size_t g_count;
static int g_epfd = -1;
static uint32_t g_next_id = 1;

static conn *g_flush_head;   /* 이번 반복에 보낼 것이 생긴 연결 */
static conn *g_close_head;   /* 종료 표시된 연결 */
static conn *g_by_token;     /* uthash(hh_token): udp_token → 연결 */
static size_t g_udp_bound;

int conn_system_init(int epfd, size_t max_fds)
{
    g_table = calloc(max_fds, sizeof *g_table);
    if (g_table == NULL)
        return -1;
    g_table_size = max_fds;
    g_epfd = epfd;
    return 0;
}

size_t conn_count(void)
{
    return g_count;
}

conn *conn_find_by_token(uint32_t token)
{
    conn *c;
    HASH_FIND(hh_token, g_by_token, &token, sizeof token, c);
    return c;
}

void conn_udp_bind(conn *c, const struct sockaddr_in *from)
{
    if (!c->udp_bound)
        g_udp_bound++;
    c->udp_bound = true;
    c->udp_addr = *from;
}

size_t conn_udp_bound_count(void)
{
    return g_udp_bound;
}

static void set_epollout(conn *c, bool on)
{
    if (c->epollout_on == on)
        return;
    /* LT에서 EPOLLOUT을 켠 채로 두면 커널 송신 버퍼에 자리가 있는 한 epoll_wait이 매번
     * 바로 깨어나 CPU를 다 쓴다. 남은 바이트가 있을 때만 켠다. */
    struct epoll_event ev = {.events = EPOLLIN | (on ? EPOLLOUT : 0u), .data.ptr = c};
    if (epoll_ctl(g_epfd, EPOLL_CTL_MOD, c->fd, &ev) == -1) {
        conn_kill(c, "epoll_ctl mod failed");
        return;
    }
    c->epollout_on = on;
}

conn *conn_open(int fd)
{
    if (fd < 0 || (size_t)fd >= g_table_size)
        return NULL;
    conn *c = calloc(1, sizeof *c);
    if (c == NULL)
        return NULL;
    c->fd = fd;
    c->slot = -1;
    frame_reader_init(&c->in);
    struct epoll_event ev = {.events = EPOLLIN, .data.ptr = c};
    if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        free(c);
        return NULL;
    }
    c->id = g_next_id++;
    if (g_next_id == 0) /* 0은 "없음"으로 읽힐 수 있어 건너뛴다 */
        g_next_id = 1;
    /* 토큰은 UDP 데이터그램의 주인을 가리는 유일한 근거라 추측하기 어려운 무작위 값이어야 하고,
     * 두 연결이 같은 값을 가지면 한쪽 입력이 다른 쪽으로 가므로 겹치면 다시 뽑는다. */
    conn *dup;
    do {
        rng_fill(&c->udp_token, sizeof c->udp_token);
        HASH_FIND(hh_token, g_by_token, &c->udp_token, sizeof c->udp_token, dup);
    } while (dup != NULL);
    HASH_ADD(hh_token, g_by_token, udp_token, sizeof c->udp_token, c);
    g_table[fd] = c;
    g_count++;
    return c;
}

void conn_kill(conn *c, const char *why)
{
    if (c->closing)
        return;
    c->closing = true;
    c->close_reason = why;
    c->close_next = g_close_head;
    g_close_head = c;
}

static void flush_unlink(conn *c)
{
    if (!c->in_flush)
        return;
    if (c->flush_prev != NULL)
        c->flush_prev->flush_next = c->flush_next;
    else
        g_flush_head = c->flush_next;
    if (c->flush_next != NULL)
        c->flush_next->flush_prev = c->flush_prev;
    c->flush_prev = c->flush_next = NULL;
    c->in_flush = false;
}

uint8_t *conn_reserve(conn *c, size_t n)
{
    if (c->closing)
        return NULL;
    size_t pending = c->out_len - c->out_off;
    if (pending + n > CONN_OUT_MAX) {
        conn_kill(c, "send buffer over 64KB");
        return NULL;
    }
    if (c->out_len + n > c->out_cap) {
        /* 앞에 이미 보낸 자리가 있으면 당겨서 쓰고, 그래도 모자라면 늘린다. */
        if (c->out_off > 0) {
            memmove(c->out, c->out + c->out_off, pending);
            c->out_off = 0;
            c->out_len = pending;
        }
        if (c->out_len + n > c->out_cap) {
            size_t cap = c->out_cap ? c->out_cap : OUT_INITIAL_CAP;
            while (cap < c->out_len + n)
                cap *= 2;
            if (cap > CONN_OUT_MAX)
                cap = CONN_OUT_MAX;
            uint8_t *grown = realloc(c->out, cap);
            if (grown == NULL) {
                conn_kill(c, "out of memory");
                return NULL;
            }
            c->out = grown;
            c->out_cap = cap;
        }
    }
    uint8_t *dst = c->out + c->out_len;
    c->out_len += n;
    if (!c->in_flush) {
        c->in_flush = true;
        c->flush_prev = NULL;
        c->flush_next = g_flush_head;
        if (g_flush_head != NULL)
            g_flush_head->flush_prev = c;
        g_flush_head = c;
    }
    return dst;
}

void conn_send_frame(conn *c, uint8_t type, const uint8_t *payload, size_t len)
{
    uint8_t *dst = conn_reserve(c, FRAME_HEADER_LEN + len);
    if (dst == NULL)
        return;
    frame_put_header(dst, type, (uint16_t)len);
    if (len > 0)
        memcpy(dst + FRAME_HEADER_LEN, payload, len);
    g_stats.tcp_tx_frames++;
}

/* 커널이 받아 주는 만큼 보낸다. 다 보냈거나 EAGAIN이면 0, 소켓 오류면 -1. */
static int write_raw(conn *c)
{
    while (c->out_off < c->out_len) {
        /* MSG_NOSIGNAL: 상대가 이미 닫은 소켓에 쓰면 SIGPIPE로 서버가 죽는다. 대신 EPIPE를 받는다. */
        ssize_t n = send(c->fd, c->out + c->out_off, c->out_len - c->out_off, MSG_NOSIGNAL);
        if (n > 0) {
            c->out_off += (size_t)n;
            g_stats.tcp_tx_bytes += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else {
            return -1;
        }
    }
    if (c->out_off == c->out_len)
        c->out_off = c->out_len = 0;
    return 0;
}

static void write_pending(conn *c)
{
    if (write_raw(c) == -1)
        conn_kill(c, "send failed");
}

static void flush_one(conn *c)
{
    write_pending(c);
    if (!c->closing)
        set_epollout(c, c->out_len > 0);
}

void conn_flush_all(void)
{
    while (g_flush_head != NULL) {
        conn *c = g_flush_head;
        flush_unlink(c);
        if (!c->closing)
            flush_one(c);
    }
}

void conn_on_writable(conn *c)
{
    if (!c->closing)
        flush_one(c);
}

static void destroy(conn *c)
{
    flush_unlink(c);
    /* epoll에서 먼저 빼야 같은 fd 번호가 다시 쓰일 때 옛 등록이 남지 않는다. close만으로도
     * 빠지지만, dup된 fd가 있으면 남으므로 명시적으로 지운다. */
    epoll_ctl(g_epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    g_table[c->fd] = NULL;
    HASH_DELETE(hh_token, g_by_token, c);
    if (c->udp_bound)
        g_udp_bound--;
    g_count--;
    free(c->out);
    free(c);
}

bool conn_sweep(void (*on_close)(conn *c))
{
    bool any = false;
    while (g_close_head != NULL) {
        conn *c = g_close_head;
        g_close_head = c->close_next;
        any = true;
        if (on_close != NULL)
            on_close(c);
        /* 끊기 전에 넣어 둔 마지막 메시지(ERROR(TOO_MANY_CODE_TRIES) 등)를 한 번 더 내보내 본다.
         * 논블로킹이라 커널이 받지 않으면 그냥 버린다. 오류여도 이미 닫는 중이라 무시한다. */
        (void)write_raw(c);
        if (!g_quiet_logs)
            fprintf(stderr, "conn %u closed: %s\n", c->id, c->close_reason);
        destroy(c);
    }
    return any;
}

void conn_system_shutdown(void)
{
    for (size_t fd = 0; fd < g_table_size; fd++) {
        if (g_table[fd] != NULL)
            destroy(g_table[fd]);
    }
    g_close_head = NULL;
    g_flush_head = NULL;
    free(g_table);
    g_table = NULL;
    g_table_size = 0;
}
