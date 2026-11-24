/* server.c - epoll(LT) 단일 스레드 메인 루프.
 *
 * 한 바퀴: 기다림 → 받은 바이트를 모두 메시지로 잘라 처리 → 진행 시각이 된 방 진행
 * → 종료 표시된 연결 정리 → 쌓인 송신 버퍼 내보내기 (overview.md 6.2).
 */
#include "server.h"

#include <errno.h>
#include <fcntl.h>
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
#include "stats.h"
#include "tick.h"
#include "udp.h"
#include "wire.h"

#define MAX_EVENTS 64
/* 한 연결에서 한 번 깨어날 때 recv하는 횟수 상한. 쉬지 않고 보내는 클라이언트 하나가 다른
 * 연결과 진행을 굶기지 않게 한다. LT라 남은 바이트는 다음 바퀴에 다시 알려 준다. */
#define MAX_READS_PER_WAKE 16
/* fd 상한에서 남겨 두는 여유(리스닝 · UDP · epoll · 예비 fd · statm · 표준 입출력 · 로그). */
#define FD_RESERVE 16
#define FD_TABLE_CAP (1u << 20)
/* fd가 바닥났는데 예비 fd도 없을 때 accept를 쉬는 시간. 그동안 LT가 매 바퀴 깨우지 않는다. */
#define ACCEPT_PAUSE_NS (100u * NS_PER_MS)

static volatile sig_atomic_t g_stop;
static size_t g_max_conns;
static int g_epfd = -1, g_lfd = -1, g_ufd = -1;
static int g_spare_fd = -1;    /* fd가 바닥났을 때 잠깐 내주는 예비 fd */
static bool g_accept_paused;
static uint64_t g_accept_resume_ns;

/* epoll data.ptr: 연결이면 conn 포인터, 듣는 소켓 둘은 이 표식의 주소. */
static char g_listen_tag, g_udp_tag;

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

/* fd 소프트 상한(배포 VM은 흔히 1024)을 하드 상한까지 올리고 그 값을 연결 표 크기로 쓴다.
 * 소프트 상한에 머물면 부하 측정이 1024 접속 언저리에서 막힌다. */
static size_t fd_table_size(void)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == -1)
        return FD_TABLE_CAP;
    rlim_t want = rl.rlim_max == RLIM_INFINITY || rl.rlim_max > FD_TABLE_CAP ? FD_TABLE_CAP
                                                                             : rl.rlim_max;
    if (rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < want) {
        struct rlimit raised = {.rlim_cur = want, .rlim_max = rl.rlim_max};
        if (setrlimit(RLIMIT_NOFILE, &raised) == 0)
            rl.rlim_cur = want;
    }
    if (rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur > FD_TABLE_CAP)
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
    g_stats.conns_rejected++;
}

static void set_listen_interest(bool on)
{
    struct epoll_event ev = {.events = on ? EPOLLIN : 0u, .data.ptr = &g_listen_tag};
    if (epoll_ctl(g_epfd, EPOLL_CTL_MOD, g_lfd, &ev) == -1)
        perror("epoll_ctl(listen)");
    g_accept_paused = !on;
}

/* fd가 바닥나 accept가 EMFILE · ENFILE로 실패하면 기다리는 연결이 대기열에 남고, LT epoll은
 * 그 소켓을 매 바퀴 다시 알려 CPU를 다 쓴다. 예비 fd를 닫아 자리를 만들어 하나를 받아
 * SERVER_FULL로 돌려보내고 예비 fd를 다시 잡는다. 하나를 받아 냈으면 1, 대기열이 비었으면 0,
 * 예비 fd가 없거나 받기도 실패하면 -1(errno는 그 이유). */
static int shed_one(void)
{
    if (g_spare_fd == -1)
        return -1;
    close(g_spare_fd);
    g_spare_fd = -1;
    int fd = accept4(g_lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    int err = errno; /* reject_full · open이 errno를 바꾼다 */
    if (fd != -1)
        reject_full(fd);
    g_spare_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd != -1)
        return 1;
    errno = err;
    return err == EAGAIN || err == EWOULDBLOCK ? 0 : -1;
}

static void accept_all(uint64_t now)
{
    for (;;) {
        int fd = accept4(g_lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd == -1) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            if (errno == EMFILE || errno == ENFILE) {
                int shed = shed_one();
                if (shed == 1)
                    continue;
                if (shed == 0) /* 기다리는 연결을 모두 돌려보냈다 */
                    return;
            }
            /* 예비 fd도 없거나 다른 자원 부족(ENOBUFS 등): 잠시 듣기를 멈춰 바쁜 루프를 피한다. */
            if (!g_quiet_logs)
                perror("accept4");
            set_listen_interest(false);
            g_accept_resume_ns = now + ACCEPT_PAUSE_NS;
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
    while (!c->closing && (st = frame_reader_next(&c->in, &f)) == FRAME_OK) {
        g_stats.tcp_rx_frames++;
        session_handle(c, &f);
    }
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
            g_stats.tcp_rx_bytes += (uint64_t)n;
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

/* epoll 타임아웃 둘 중 가까운 쪽. -1은 "기다릴 시각 없음". */
static int sooner(int a, int b)
{
    if (a < 0)
        return b;
    if (b < 0)
        return a;
    return a < b ? a : b;
}

static int wait_timeout_ms(uint64_t now)
{
    int t = sooner(rooms_timeout_ms(now), stats_timeout_ms(now));
    if (g_accept_paused)
        t = sooner(t, tick_ms_until(g_accept_resume_ns, now));
    return t;
}

/* 한 바퀴의 2–5단계(overview.md 6.2). */
static void handle_events(const struct epoll_event *events, int n, uint64_t woke)
{
    for (int i = 0; i < n; i++) {
        void *p = events[i].data.ptr;
        if (p == &g_listen_tag) {
            accept_all(woke);
            continue;
        }
        if (p == &g_udp_tag) {
            udp_on_readable();
            continue;
        }
        conn *c = p;
        if (c->closing)
            continue;
        if (events[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR))
            on_readable(c);
        if ((events[i].events & EPOLLOUT) && !c->closing)
            conn_on_writable(c);
    }
    if (g_accept_paused && tick_now_ns() >= g_accept_resume_ns)
        set_listen_interest(true);

    uint64_t now = tick_now_ns();
    if (stats_enabled()) {
        uint64_t ticks_before = g_stats.ticks;
        rooms_tick(now);
        if (g_stats.ticks != ticks_before)
            stats_record_tick_ns(tick_now_ns() - now);
    } else {
        rooms_tick(now);
    }
    sweep_and_flush();
}

/* 듣는 소켓 · epoll · 표를 준비한다. 실패하면 -1(연 것은 server_close가 닫는다). */
static int server_open(const server_config *cfg)
{
    uint16_t bound;
    if (net_open(cfg->port, cfg->udp, &g_lfd, &g_ufd, &bound) == -1)
        return -1;
    g_epfd = epoll_create1(EPOLL_CLOEXEC);
    if (g_epfd == -1) {
        perror("epoll_create1");
        return -1;
    }
    size_t table = fd_table_size();
    size_t limit = table > FD_RESERVE ? table - FD_RESERVE : 1;
    g_max_conns = cfg->max_conns != 0 ? cfg->max_conns : limit;
    struct epoll_event lev = {.events = EPOLLIN, .data.ptr = &g_listen_tag};
    struct epoll_event uev = {.events = EPOLLIN, .data.ptr = &g_udp_tag};
    if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, g_lfd, &lev) == -1 ||
        (g_ufd != -1 && epoll_ctl(g_epfd, EPOLL_CTL_ADD, g_ufd, &uev) == -1) ||
        conn_system_init(g_epfd, table) == -1 || rooms_init() == -1) {
        perror("init");
        return -1;
    }
    g_spare_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (g_spare_fd == -1) {
        perror("open(/dev/null)");
        return -1;
    }
    udp_set_socket(g_ufd);
    g_quiet_logs = cfg->quiet;
    stats_init(cfg->stats_interval, tick_now_ns());
    fprintf(stderr, "max conns %zu, udp %s, stats every %u s%s\n", g_max_conns,
            g_ufd != -1 ? "on" : "off", cfg->stats_interval, cfg->quiet ? ", quiet" : "");
    /* 테스트와 배포 스크립트가 --port 0일 때 실제 포트를 읽는다. 파이프면 줄 버퍼가 아니므로 flush. */
    printf("listening on port %u\n", (unsigned)bound);
    fflush(stdout);
    return 0;
}

static void close_fd(int *fd)
{
    if (*fd != -1)
        close(*fd);
    *fd = -1;
}

static void server_close(void)
{
    rooms_shutdown();
    conn_system_shutdown();
    stats_shutdown();
    udp_set_socket(-1);
    close_fd(&g_spare_fd);
    close_fd(&g_epfd);
    close_fd(&g_ufd);
    close_fd(&g_lfd);
}

int server_run(const server_config *cfg)
{
    sigset_t wait_mask;
    if (setup_signals(&wait_mask) == -1) {
        perror("signals");
        return 1;
    }
    if (server_open(cfg) == -1) {
        server_close();
        return 1;
    }
    struct epoll_event events[MAX_EVENTS];
    while (!g_stop) {
        int n = epoll_pwait(g_epfd, events, MAX_EVENTS, wait_timeout_ms(tick_now_ns()), &wait_mask);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            perror("epoll_pwait");
            break;
        }
        uint64_t woke = tick_now_ns();
        handle_events(events, n, woke);
        uint64_t done = tick_now_ns();
        if (stats_enabled()) {
            stats_record_loop_ns(done - woke);
            stats_maybe_report(done);
        }
    }
    fprintf(stderr, "shutting down: %zu connections, %zu rooms\n", conn_count(), rooms_count());
    server_close();
    return 0;
}
