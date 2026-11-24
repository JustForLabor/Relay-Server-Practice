/* main.c - 인자 처리 후 메인 루프를 돈다. 옵션은 usage 참고. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"

#define DEFAULT_PORT 7777

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [--port N] [--no-udp] [--max-conns N] [--stats-interval SEC] [--quiet]\n"
            "  --port N              TCP and UDP port, 0 = any free port (default %d)\n"
            "  --no-udp              TCP only (protocol.md 7)\n"
            "  --max-conns N         concurrent connection limit (default: fd limit - reserve)\n"
            "  --stats-interval SEC  print one JSON stats line to stdout every SEC (default 0 = off)\n"
            "  --quiet               no per-connection / per-room log lines on stderr\n",
            prog, DEFAULT_PORT);
}

/* 10진 정수 하나. 빈 문자열 · 남는 글자 · 범위 밖이면 0. */
static int parse_num(const char *s, long lo, long hi, long *out)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || v < lo || v > hi)
        return 0;
    *out = v;
    return 1;
}

int main(int argc, char **argv)
{
    server_config cfg = {.port = DEFAULT_PORT, .udp = true};
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        long v;
        if (strcmp(a, "--no-udp") == 0) {
            cfg.udp = false;
        } else if (strcmp(a, "--quiet") == 0) {
            cfg.quiet = true;
        } else if (i + 1 < argc && strcmp(a, "--port") == 0 && parse_num(argv[i + 1], 0, 65535, &v)) {
            cfg.port = (uint16_t)v;
            i++;
        } else if (i + 1 < argc && strcmp(a, "--max-conns") == 0 &&
                   parse_num(argv[i + 1], 1, 1L << 30, &v)) {
            cfg.max_conns = (size_t)v;
            i++;
        } else if (i + 1 < argc && strcmp(a, "--stats-interval") == 0 &&
                   parse_num(argv[i + 1], 0, 86400, &v)) {
            cfg.stats_interval = (unsigned)v;
            i++;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    return server_run(&cfg);
}
