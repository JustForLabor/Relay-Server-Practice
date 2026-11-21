/* unit_check.h - 코어 단위 테스트가 함께 쓰는 검사 매크로.
 * 실패해도 바로 멈추지 않고 모두 센 뒤, UNIT_DONE이 실패가 있으면 1을 돌려준다. */
#ifndef UNIT_CHECK_H
#define UNIT_CHECK_H

#include <stdio.h>

static int g_checks, g_fails;

#define CHECK(c)                                                              \
    do {                                                                      \
        g_checks++;                                                           \
        if (!(c)) {                                                           \
            g_fails++;                                                        \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
        }                                                                     \
    } while (0)

#define CHECK_EQ(a, b)                                                        \
    do {                                                                      \
        long long va_ = (long long)(a), vb_ = (long long)(b);                 \
        g_checks++;                                                           \
        if (va_ != vb_) {                                                     \
            g_fails++;                                                        \
            fprintf(stderr, "%s:%d: %s == %s failed (%lld != %lld)\n", __FILE__, \
                    __LINE__, #a, #b, va_, vb_);                              \
        }                                                                     \
    } while (0)

#define UNIT_DONE(name)                                                       \
    (printf("%s: %d checks, %d failed\n", name, g_checks, g_fails), g_fails ? 1 : 0)

#endif /* UNIT_CHECK_H */
