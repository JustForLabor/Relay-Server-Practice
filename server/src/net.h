/* net.h - 리스닝 소켓과 소켓 옵션 (overview.md 6.1 net). */
#ifndef NET_H
#define NET_H

#include <stdint.h>

/* port(0이면 커널이 고름)에서 듣는 논블로킹 TCP 소켓을 만든다. 실제 포트를 *bound_port에 넣는다.
 * 실패하면 -1. */
int net_listen(uint16_t port, uint16_t *bound_port);

/* 받은 연결에 TCP_NODELAY를 켠다. 실패해도 연결은 쓸 수 있으므로 결과만 돌려준다. */
int net_set_nodelay(int fd);

#endif /* NET_H */
