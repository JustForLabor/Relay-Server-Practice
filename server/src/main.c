/* main.c - 인자 처리 후 메인 루프를 돈다.
 *   relay-server [--port N]   N이 0이면 커널이 빈 포트를 고르고 stdout에 알린다. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"

#define DEFAULT_PORT 7777

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s [--port N]   (N 0-65535, 0 = any free port, default %d)\n", prog,
            DEFAULT_PORT);
}

int main(int argc, char **argv)
{
    long port = DEFAULT_PORT;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            char *end;
            port = strtol(argv[++i], &end, 10);
            if (*argv[i] == '\0' || *end != '\0' || port < 0 || port > 65535) {
                usage(argv[0]);
                return 2;
            }
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    return server_run((uint16_t)port);
}
