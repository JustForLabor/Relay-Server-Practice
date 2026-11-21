/* wire.h - big-endian 정수 읽기 · 쓰기 (protocol.md 2장).
 *
 * 구조체를 그대로 memcpy해 보내지 않고 바이트를 하나씩 놓는 이유: 패딩과 호스트 바이트 순서가
 * 컴파일러 · CPU마다 달라 C#(클라이언트)과 같은 바이트를 보장할 수 없기 때문이다.
 */
#ifndef WIRE_H
#define WIRE_H

#include <stdint.h>

static inline void wire_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void wire_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline uint16_t wire_get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static inline uint32_t wire_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

#endif /* WIRE_H */
