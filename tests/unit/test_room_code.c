/* test_room_code.c - 비공개 방 입장 코드 생성 (protocol.md 5.3절).
 * 32글자 알파벳 밖 글자(0 · 1 · O · I 등)가 절대 나오지 않고, 32글자가 고르게 쓰이는지 본다.
 * 치우치면 추측해야 할 경우의 수가 줄어 남의 방에 들어가기 쉬워진다. */
#include <string.h>

#include "room_code.h"
#include "unit_check.h"

#define SAMPLES 5000

static int index_of(char ch)
{
    const char *a = ROOM_CODE_ALPHABET;
    const char *p = strchr(a, ch);
    return (p == NULL || ch == '\0') ? -1 : (int)(p - a);
}

int main(void)
{
    static char codes[SAMPLES][ROOM_CODE_LEN];
    int counts[ROOM_CODE_ALPHABET_LEN] = {0};
    int outside = 0;
    for (int i = 0; i < SAMPLES; i++) {
        room_code_generate(codes[i]);
        for (int k = 0; k < ROOM_CODE_LEN; k++) {
            int idx = index_of(codes[i][k]);
            if (idx < 0)
                outside++;
            else
                counts[idx]++;
            CHECK(room_code_char_ok((uint8_t)codes[i][k]));
        }
    }
    CHECK_EQ(outside, 0);

    /* 글자마다 기대값 937.5, 표준편차 약 30. ±20%(약 6 표준편차) 밖이면 생성이 치우친 것이다. */
    double expect = (double)SAMPLES * ROOM_CODE_LEN / ROOM_CODE_ALPHABET_LEN;
    for (int i = 0; i < ROOM_CODE_ALPHABET_LEN; i++) {
        CHECK(counts[i] > expect * 0.8);
        CHECK(counts[i] < expect * 1.2);
    }

    /* 32^6 중 5000개라 겹침 기대값은 0.01. 여러 개 겹치면 난수가 아니다. */
    int dups = 0;
    for (int i = 0; i < SAMPLES; i++) {
        for (int j = i + 1; j < SAMPLES; j++) {
            if (memcmp(codes[i], codes[j], ROOM_CODE_LEN) == 0)
                dups++;
        }
    }
    CHECK(dups <= 2);

    CHECK_EQ(strlen(ROOM_CODE_ALPHABET), ROOM_CODE_ALPHABET_LEN);
    const char excluded[] = "01OIoi az-_";
    for (size_t i = 0; i < sizeof excluded - 1; i++)
        CHECK(!room_code_char_ok((uint8_t)excluded[i]));
    CHECK(!room_code_char_ok(0));
    CHECK(!room_code_char_ok(0xFF));
    for (int i = 0; i < ROOM_CODE_ALPHABET_LEN; i++)
        CHECK(room_code_char_ok((uint8_t)ROOM_CODE_ALPHABET[i]));

    return UNIT_DONE("test_room_code");
}
