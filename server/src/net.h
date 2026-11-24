/* net.h - 리스닝 소켓과 소켓 옵션 (overview.md 6.1 net). */
#ifndef NET_H
#define NET_H

#include <stdbool.h>
#include <stdint.h>

/* port(0이면 커널이 고름)에서 듣는 논블로킹 TCP 소켓을 만든다. 실제 포트를 *bound_port에 넣는다.
 * 실패하면 -1. */
int net_listen(uint16_t port, uint16_t *bound_port);

/* port에 묶인 논블로킹 UDP 소켓. 실패하면 -1(이유는 errno, 출력은 호출한 쪽). */
int net_udp_open(uint16_t port);

/* TCP와 같은 번호의 UDP를 함께 연다(protocol.md 7장). port가 0이면 커널이 고른 TCP 번호가 UDP에서
 * 이미 쓰이고 있을 수 있어 몇 번 다시 고른다. udp가 false면 *ufd는 -1. 실패하면 -1, 성공 0. */
int net_open(uint16_t port, bool udp, int *lfd, int *ufd, uint16_t *bound_port);

/* 받은 연결에 TCP_NODELAY를 켠다. 실패해도 연결은 쓸 수 있으므로 결과만 돌려준다. */
int net_set_nodelay(int fd);

#endif /* NET_H */
