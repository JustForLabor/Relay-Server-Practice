/* channel.h - game_host 구현: 게임이 고른 보내기 방식을 실제 전송으로 바꾼다
 * (overview.md 6.1 channel, ADR 0001 3번). 신뢰는 TCP, 비신뢰는 UDP로 묶인 연결이면 UDP(udp.c). */
#ifndef CHANNEL_H
#define CHANNEL_H

#include "room.h"

/* r->host를 이 방을 ctx로 하는 함수 표로 채운다. */
void channel_bind(room *r);

#endif /* CHANNEL_H */
