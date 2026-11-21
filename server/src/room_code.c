/* room_code.c - 입장 코드 만들기. */
#include "room_code.h"

#include "rng.h"

static const char k_alphabet[] = ROOM_CODE_ALPHABET;

_Static_assert(sizeof k_alphabet - 1 == ROOM_CODE_ALPHABET_LEN, "alphabet must be 32 chars");

void room_code_generate(char out[ROOM_CODE_LEN])
{
    uint8_t raw[ROOM_CODE_LEN];
    rng_fill(raw, sizeof raw);
    for (int i = 0; i < ROOM_CODE_LEN; i++)
        out[i] = k_alphabet[raw[i] & (ROOM_CODE_ALPHABET_LEN - 1)];
}

bool room_code_char_ok(uint8_t ch)
{
    for (int i = 0; i < ROOM_CODE_ALPHABET_LEN; i++) {
        if ((uint8_t)k_alphabet[i] == ch)
            return true;
    }
    return false;
}
