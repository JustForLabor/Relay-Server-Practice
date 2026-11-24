/* udp.h - UDP 소켓 하나로 하는 묶기 · 비신뢰 GAME_OUT 보내기 · GAME_IN 받기 (protocol.md 7장).
 *
 * TCP와 같은 포트 번호의 UDP 소켓 하나를 모든 연결이 같이 쓴다. 데이터그램의 주인은 앞 4바이트
 * udp_token으로 찾고, GAME_IN은 그 연결이 묶인 주소에서 온 것만 받는다. 어긋난 데이터그램은
 * 출처를 믿을 수 없으므로 답하지도 연결을 끊지도 않고 버린다.
 */
#ifndef UDP_H
#define UDP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "conn.h"

/* 이미 열어 epoll에 등록한 UDP 소켓을 쓴다. -1이면 UDP를 쓰지 않는다(--no-udp). */
void udp_set_socket(int fd);

/* 소켓에 온 데이터그램을 처리한다(한 번에 상한까지, 남은 것은 LT가 다시 알린다). */
void udp_on_readable(void);

/* c가 UDP로 묶여 있으면 프레임 하나를 데이터그램으로 보내고 true. 보내기가 실패해도(EAGAIN 등)
 * 비신뢰 메시지라 버리고 true다. 묶이지 않았거나 UDP를 끈 서버면 false(호출한 쪽이 TCP로 보낸다). */
bool udp_send_frame(conn *c, uint8_t type, const uint8_t *payload, size_t len);

#endif /* UDP_H */
