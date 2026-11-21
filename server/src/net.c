/* net.c - 리스닝 소켓 만들기. */
#include "net.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define LISTEN_BACKLOG 128

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
