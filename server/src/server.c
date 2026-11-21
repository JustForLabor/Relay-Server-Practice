/* server.c - epoll(LT) 단일 스레드 메인 루프.
 *
 * 한 바퀴: 기다림 → 받은 바이트를 모두 메시지로 잘라 처리 → 진행 시각이 된 방 진행
 * → 종료 표시된 연결 정리 → 쌓인 송신 버퍼 내보내기 (overview.md 6.2).
 */
#include "server.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include "conn.h"
#include "frame.h"
#include "msg.h"
#include "net.h"
#include "room.h"
#include "session.h"
#include "tick.h"
#include "wire.h"

#define MAX_EVENTS 64
/* 한 연결에서 한 번 깨어날 때 recv하는 횟수 상한. 쉬지 않고 보내는 클라이언트 하나가 다른
 * 연결과 진행을 굶기지 않게 한다. LT라 남은 바이트는 다음 바퀴에 다시 알려 준다. */
#define MAX_READS_PER_WAKE 16
/* fd 상한에서 남겨 두는 여유(리스닝 · epoll · 표준 입출력 · 로그). */
#define FD_RESERVE 16
#define FD_TABLE_CAP (1u << 20)

static volatile sig_atomic_t g_stop;
static size_t g_max_conns;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* SIGINT · SIGTERM은 평소에 막아 두고 epoll_pwait 안에서만 받는다. 그러면 "g_stop 검사 뒤,
 * epoll_wait 진입 전"에 온 시그널을 놓쳐 다음 진행 시각까지 잠드는 경쟁이 없다. */
static int setup_signals(sigset_t *wait_mask)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) == -1 || sigaction(SIGTERM, &sa, NULL) == -1)
        return -1;
    sa.sa_handler = SIG_IGN; /* send에 MSG_NOSIGNAL을 쓰지만, 혹시 모를 write 경로를 막는다 */
    if (sigaction(SIGPIPE, &sa, NULL) == -1)
        return -1;
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    return sigprocmask(SIG_BLOCK, &block, wait_mask);
}

static size_t fd_table_size(void)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == -1 || rl.rlim_cur == RLIM_INFINITY ||
        rl.rlim_cur > FD_TABLE_CAP)
        return FD_TABLE_CAP;
    return (size_t)rl.rlim_cur;
}

/* 동시 접속 상한을 넘은 새 연결: 연결 표를 만들지 않고 ERROR(SERVER_FULL)만 써 보고 닫는다. */
static void reject_full(int fd)
{
    static const char reason[] = "server full";
    uint8_t f[FRAME_HEADER_LEN + 2 + sizeof reason - 1];
    frame_put_header(f, MSG_ERROR, (uint16_t)(sizeof f - FRAME_HEADER_LEN));
    wire_put_u16(f + FRAME_HEADER_LEN, ERR_SERVER_FULL);
    memcpy(f + FRAME_HEADER_LEN + 2, reason, sizeof reason - 1);
    (void)send(fd, f, sizeof f, MSG_NOSIGNAL | MSG_DONTWAIT);
    close(fd);
}

static void accept_all(int lfd)
{
    for (;;) {
        int fd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd == -1) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                perror("accept4");
            return;
        }
        if (net_set_nodelay(fd) == -1)
            perror("setsockopt(TCP_NODELAY)");
        if (conn_count() >= g_max_conns) {
            reject_full(fd);
            continue;
        }
        conn *c = conn_open(fd);
        if (c == NULL) {
            reject_full(fd);
            continue;
        }
        session_open(c);
    }
}

/* 쌓인 바이트에서 완성된 프레임을 모두 꺼내 처리한다. */
static void drain_frames(conn *c)
{
    frame f;
    frame_status st;
    while (!c->closing && (st = frame_reader_next(&c->in, &f)) == FRAME_OK)
        session_handle(c, &f);
    if (!c->closing && st == FRAME_BAD)
        conn_kill(c, c->in.err);
}

static void on_readable(conn *c)
{
    for (int i = 0; i < MAX_READS_PER_WAKE && !c->closing; i++) {
        size_t avail;
        uint8_t *dst = frame_reader_space(&c->in, &avail);
        ssize_t n = recv(c->fd, dst, avail, 0);
        if (n > 0) {
            frame_reader_commit(&c->in, (size_t)n);
            drain_frames(c);
            if ((size_t)n < avail) /* 커널 버퍼를 다 비웠다 */
                return;
        } else if (n == 0) {
            conn_kill(c, "peer closed");
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        } else {
            conn_kill(c, "recv failed");
        }
    }
}

static void on_close(conn *c)
{
    room_leave(c);
}

/* 종료 표시된 연결을 정리하고 송신 버퍼를 내보낸다. 정리 중 보낸 PLAYER_LEFT 등이 이번
 * 바퀴에 나가야 하고, 내보내다 실패한 연결은 다시 정리해야 하므로 둘 다 할 일이 없을 때까지 돈다. */
static void sweep_and_flush(void)
{
    for (;;) {
        conn_sweep(on_close);
        conn_flush_all();
        if (!conn_sweep(on_close))
            return;
    }
}

int server_run(uint16_t port)
{
    sigset_t wait_mask;
    if (setup_signals(&wait_mask) == -1) {
        perror("signals");
        return 1;
    }
    uint16_t bound;
    int lfd = net_listen(port, &bound);
    if (lfd == -1)
        return 1;
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1) {
        perror("epoll_create1");
        close(lfd);
        return 1;
    }
    size_t table = fd_table_size();
    g_max_conns = table > FD_RESERVE ? table - FD_RESERVE : 1;
    struct epoll_event lev = {.events = EPOLLIN, .data.ptr = NULL};
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &lev) == -1 || conn_system_init(epfd, table) == -1 ||
        rooms_init() == -1) {
        perror("init");
        conn_system_shutdown();
        close(epfd);
        close(lfd);
        return 1;
    }
    /* 테스트와 배포 스크립트가 --port 0일 때 실제 포트를 읽는다. 파이프면 줄 버퍼가 아니므로 flush. */
    printf("listening on port %u\n", (unsigned)bound);
    fflush(stdout);

    struct epoll_event events[MAX_EVENTS];
    while (!g_stop) {
        int timeout = rooms_timeout_ms(tick_now_ns());
        int n = epoll_pwait(epfd, events, MAX_EVENTS, timeout, &wait_mask);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            perror("epoll_pwait");
            break;
        }
        for (int i = 0; i < n; i++) {
            conn *c = events[i].data.ptr;
            if (c == NULL) {
                accept_all(lfd);
                continue;
            }
            if (c->closing)
                continue;
            if (events[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR))
                on_readable(c);
            if ((events[i].events & EPOLLOUT) && !c->closing)
                conn_on_writable(c);
        }
        rooms_tick(tick_now_ns());
        sweep_and_flush();
    }

    fprintf(stderr, "shutting down: %zu connections, %zu rooms\n", conn_count(), rooms_count());
    rooms_shutdown();
    conn_system_shutdown();
    close(epfd);
    close(lfd);
    return 0;
}
