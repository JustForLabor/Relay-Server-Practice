/* rng.c - getrandom 래퍼. */
#include "rng.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/random.h>

void rng_fill(void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n > 0) {
        ssize_t got = getrandom(p, n, 0);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            perror("getrandom");
            abort();
        }
        p += got;
        n -= (size_t)got;
    }
}
