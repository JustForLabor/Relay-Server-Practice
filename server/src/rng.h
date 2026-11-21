/* rng.h - 예측하기 어려운 난수 (입장 코드, udp_token).
 * rand()는 시드를 알면 다음 값을 맞힐 수 있어 남의 방 코드를 추측당한다. 커널 난수를 쓴다. */
#ifndef RNG_H
#define RNG_H

#include <stddef.h>

/* buf를 커널 난수로 채운다. 커널이 난수를 주지 못하면 프로그램을 끝낸다:
 * 추측 가능한 값으로 계속 도는 것보다 멈추는 편이 안전하다. */
void rng_fill(void *buf, size_t n);

#endif /* RNG_H */
