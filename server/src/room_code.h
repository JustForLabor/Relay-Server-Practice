/* room_code.h - 비공개 방 입장 코드 (protocol.md 5.3절). */
#ifndef ROOM_CODE_H
#define ROOM_CODE_H

#include <stdbool.h>
#include <stdint.h>

#include "protocol.h"

/* 0 · 1 · O · I를 뺀 32글자. 32는 256의 약수라 난수 바이트 하위 5비트로 고르면 치우침이 없다. */
#define ROOM_CODE_ALPHABET "23456789ABCDEFGHJKLMNPQRSTUVWXYZ"
#define ROOM_CODE_ALPHABET_LEN 32

/* 커널 난수로 6글자를 만든다(NUL 없음). 겹침 검사는 방 목록이 한다. */
void room_code_generate(char out[ROOM_CODE_LEN]);

/* 코드에 쓰일 수 있는 글자인가. */
bool room_code_char_ok(uint8_t ch);

#endif /* ROOM_CODE_H */
