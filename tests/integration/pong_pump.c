/* pong_pump.c - Pong 경기를 실제 클라이언트처럼 돌리는 도우미 (protocol.md 6.1절 GAME_IN · STATE). */
#include <poll.h>
#include <string.h>

#include "scenarios.h"
#include "wire.h"

#define INPUT_INTERVAL_MS 33 /* protocol.md 6.1: MATCH_START 뒤 33ms마다 하나씩 */

static void record(pong_log *l, const msg *m)
{
    if (m->len == 20 && m->p[0] == 0x01) { /* STATE */
        if (l->states < PONG_LOG_CAP) {
            l->tick[l->states] = wire_get_u32(m->p + 1);
            l->ball_x[l->states] = wire_get_u16(m->p + 9);
            l->ball_y[l->states] = wire_get_u16(m->p + 11);
        }
        l->last_input_seq = wire_get_u32(m->p + 5);
        l->states++;
    } else if (m->len == 4 && m->p[0] == 0x02) { /* GOAL */
        l->goals++;
    }
}

int pump_pong(client *cs[], pong_log logs[], int n, int duration_ms, uint8_t stop_type)
{
    memset(logs, 0, sizeof *logs * (size_t)n);
    uint64_t start = itest_now_ms();
    uint64_t end = start + (uint64_t)duration_ms;
    uint64_t next_send = start;
    for (;;) {
        uint64_t now = itest_now_ms();
        if (now >= end)
            return 1;
        if (stop_type != 0) {
            int all = 1;
            for (int i = 0; i < n; i++)
                all = all && logs[i].stopped;
            if (all)
                return 1;
        }
        if (now >= next_send) {
            for (int i = 0; i < n; i++) {
                if (!logs[i].stopped)
                    send_pong_input(cs[i], 0);
            }
            next_send += INPUT_INTERVAL_MS;
            continue;
        }
        struct pollfd pfd[8];
        for (int i = 0; i < n; i++) {
            pfd[i].fd = cs[i]->fd;
            pfd[i].events = POLLIN;
        }
        uint64_t wake = next_send < end ? next_send : end;
        poll(pfd, (nfds_t)n, (int)(wake - now));
        for (int i = 0; i < n; i++) {
            if ((pfd[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0)
                continue;
            msg m;
            int r;
            while ((r = client_recv(cs[i], &m, 0)) == 1) {
                if (m.type == MSG_GAME_OUT)
                    record(&logs[i], &m);
                else if (m.type == stop_type && !logs[i].stopped) {
                    logs[i].stopped = 1;
                    logs[i].stop = m;
                }
            }
            if (r == -1)
                return 0;
        }
    }
}

int pong_logs_agree(const pong_log *a, const pong_log *b)
{
    int na = a->states < PONG_LOG_CAP ? a->states : PONG_LOG_CAP;
    int nb = b->states < PONG_LOG_CAP ? b->states : PONG_LOG_CAP;
    int common = 0;
    for (int i = 0; i < na; i++) {
        for (int j = 0; j < nb; j++) {
            if (a->tick[i] != b->tick[j])
                continue;
            if (a->ball_x[i] != b->ball_x[j] || a->ball_y[i] != b->ball_y[j])
                return -1;
            common++;
            break;
        }
    }
    return common;
}

int room_list_has(const msg *list, uint32_t room_id, uint8_t *players, uint8_t *max)
{
    if (list->len < 3)
        return 0;
    uint8_t count = list->p[2];
    for (int i = 0; i < count && 3 + 6 * (i + 1) <= list->len; i++) {
        const uint8_t *e = list->p + 3 + 6 * i;
        if (wire_get_u32(e) == room_id) {
            if (players != NULL)
                *players = e[4];
            if (max != NULL)
                *max = e[5];
            return 1;
        }
    }
    return 0;
}
