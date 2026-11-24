/* net.c - 리스닝 소켓 만들기. */
#include "net.h"

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define LISTEN_BACKLOG 128
/* --port 0에서 TCP · UDP 번호를 맞추려 다시 고르는 횟수. */
#define PAIR_TRIES 16

int net_listen(uint16_t port, uint16_t *bound_port)
{
    /* SOCK_NONBLOCK: epoll LT에서 accept가 EAGAIN까지 돌다가 막히지 않게 한다.
     * IPv4 모든 주소에서 듣는다(1차 배포 대상이 IPv4 VM 하나). */
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd == -1) {
        perror("socket");
        return -1;
    }
    int one = 1; /* 재시작할 때 TIME_WAIT 때문에 bind가 실패하지 않게 한다 */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) == -1) {
        perror("setsockopt(SO_REUSEADDR)");
        close(fd);
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) == -1 || listen(fd, LISTEN_BACKLOG) == -1) {
        perror("bind/listen");
        close(fd);
        return -1;
    }
    socklen_t alen = sizeof addr;
    if (getsockname(fd, (struct sockaddr *)&addr, &alen) == -1) {
        perror("getsockname");
        close(fd);
        return -1;
    }
    *bound_port = ntohs(addr.sin_port);
    return fd;
}

int net_set_nodelay(int fd)
{
    /* 작은 프레임(입력 9바이트, 상태 24바이트)을 Nagle이 묶어 늦게 보내지 않게 한다(ADR 0001). */
    int one = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

int net_udp_open(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd == -1)
        return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) == -1) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int net_open(uint16_t port, bool udp, int *lfd, int *ufd, uint16_t *bound_port)
{
    *ufd = -1;
    for (int i = 0; i < PAIR_TRIES; i++) {
        *lfd = net_listen(port, bound_port);
        if (*lfd == -1)
            return -1;
        if (!udp)
            return 0;
        *ufd = net_udp_open(*bound_port);
        if (*ufd != -1)
            return 0;
        int e = errno;
        close(*lfd);
        *lfd = -1;
        if (port != 0 || e != EADDRINUSE) {
            errno = e;
            perror("udp bind");
            return -1;
        }
    }
    fprintf(stderr, "could not find a port free for both TCP and UDP\n");
    return -1;
}
