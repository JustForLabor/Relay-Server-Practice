/* room.c - 방 목록과 입장 · 퇴장. 설명은 room.h. */
#include "room.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "channel.h"
#include "games.h"
#include "match.h"
#include "msg.h"
#include "room_code.h"
#include "wire.h"

typedef struct lobby_list {
    room *head, *tail;
} lobby_list;

static room *g_rooms;          /* uthash: id → 방 */
static room *g_by_code;        /* uthash(hh_code): 비공개 코드 → 방 */
static lobby_list *g_lobby;    /* 게임별 공개 대기 목록 */
static uint32_t g_next_room_id = 1;

int rooms_init(void)
{
    g_lobby = calloc(games_count(), sizeof *g_lobby);
    return g_lobby == NULL ? -1 : 0;
}

size_t rooms_count(void)
{
    return HASH_COUNT(g_rooms);
}

/* 공개 목록 규칙(protocol.md 5.3절): 빈자리가 있는 대기 방만. 경기 중 · 막 끝난 방은 빠진다. */
static bool listable(const room *r)
{
    return r->visibility == ROOM_VISIBILITY_PUBLIC && r->match == NULL && !r->just_ended &&
           r->count > 0 && r->count < r->game->max_players;
}

static void unlist(room *r)
{
    if (!r->listed)
        return;
    lobby_list *l = &g_lobby[r->game_index];
    if (r->list_prev != NULL)
        r->list_prev->list_next = r->list_next;
    else
        l->head = r->list_next;
    if (r->list_next != NULL)
        r->list_next->list_prev = r->list_prev;
    else
        l->tail = r->list_prev;
    r->list_prev = r->list_next = NULL;
    r->listed = false;
}

void room_update_listing(room *r)
{
    bool want = listable(r);
    if (want == r->listed)
        return;
    if (!want) {
        unlist(r);
        return;
    }
    lobby_list *l = &g_lobby[r->game_index];
    r->list_prev = l->tail;
    r->list_next = NULL;
    if (l->tail != NULL)
        l->tail->list_next = r;
    else
        l->head = r;
    l->tail = r;
    r->listed = true;
}

void room_broadcast(room *r, const conn *except, uint8_t type, const uint8_t *payload,
                    size_t len)
{
    for (int i = 0; i < GAME_MAX_SLOTS; i++) {
        conn *m = r->members[i];
        if (m != NULL && m != except)
            conn_send_frame(m, type, payload, len);
    }
}

static void destroy_room(room *r)
{
    unlist(r);
    HASH_DEL(g_rooms, r);
    if (r->visibility == ROOM_VISIBILITY_PRIVATE)
        HASH_DELETE(hh_code, g_by_code, r);
    if (r->match != NULL)
        r->game->match_destroy(r->match);
    free(r);
}

/* 방 번호는 서버가 정한다(overview.md 6.5). 1부터 늘리고, 한 바퀴 돌면 쓰는 번호를 건너뛴다. */
static uint32_t alloc_room_id(void)
{
    for (;;) {
        uint32_t id = g_next_room_id++;
        if (g_next_room_id == 0)
            g_next_room_id = 1;
        room *found;
        HASH_FIND(hh, g_rooms, &id, sizeof id, found);
        if (found == NULL)
            return id;
    }
}

/* 규칙 값을 검사해 방을 만든다. 멤버는 아직 없다. 실패하면 오류 코드. */
static int new_room(uint16_t game_id, uint8_t visibility, const uint8_t *settings,
                    size_t settings_len, room **out)
{
    size_t gi;
    const game_def *g = games_find(game_id, &gi);
    if (g == NULL)
        return ERR_UNKNOWN_GAME;
    room *r = calloc(1, sizeof *r);
    if (r == NULL)
        return ERR_SERVER_FULL;
    if (g->settings_resolve == NULL) {
        if (settings_len != 0) {
            free(r);
            return ERR_BAD_SETTINGS;
        }
    } else if (g->settings_resolve(settings, settings_len, r->settings, &r->settings_len) < 0 ||
               r->settings_len > GAME_MAX_SETTINGS) {
        free(r);
        return ERR_BAD_SETTINGS;
    }
    r->game = g;
    r->game_index = gi;
    r->visibility = visibility;
    r->id = alloc_room_id();
    channel_bind(r);
    HASH_ADD(hh, g_rooms, id, sizeof r->id, r);
    if (visibility == ROOM_VISIBILITY_PRIVATE) {
        room *dup;
        do { /* 32^6(약 10억) 중 겹칠 확률은 작지만, 겹치면 다른 방에 들어가게 되므로 다시 뽑는다 */
            room_code_generate(r->code);
            HASH_FIND(hh_code, g_by_code, r->code, ROOM_CODE_LEN, dup);
        } while (dup != NULL);
        HASH_ADD(hh_code, g_by_code, code, ROOM_CODE_LEN, r);
    }
    *out = r;
    return 0;
}

/* c를 비어 있는 가장 작은 slot에 넣고 5.2절 1–3번을 한다. 빈자리는 호출한 쪽이 확인했다. */
static void add_member(room *r, conn *c)
{
    int slot = 0;
    while (r->members[slot] != NULL)
        slot++;
    r->members[slot] = c;
    r->count++;
    c->room = r;
    c->slot = slot;
    msg_room_joined(c, r);
    uint8_t s = (uint8_t)slot;
    room_broadcast(r, c, MSG_PLAYER_JOINED, &s, 1);
    if (r->match != NULL)
        match_player_joined(r, slot);
    else if (!r->just_ended && r->count >= r->game->min_players)
        match_start(r);
    room_update_listing(r);
}

static int join(room *r, conn *c)
{
    if (r->count >= r->game->max_players)
        return ERR_ROOM_FULL;
    add_member(r, c);
    return 0;
}

int room_create(conn *c, uint16_t game_id, uint8_t visibility, const uint8_t *settings,
                size_t settings_len)
{
    room *r;
    int err = new_room(game_id, visibility, settings, settings_len, &r);
    if (err != 0)
        return err;
    add_member(r, c);
    return 0;
}

int room_quick_join(conn *c, uint16_t game_id)
{
    size_t gi;
    if (games_find(game_id, &gi) == NULL)
        return ERR_UNKNOWN_GAME;
    room *r = g_lobby[gi].head;
    if (r != NULL)
        return join(r, c);
    /* 빠른 입장으로 새로 만드는 방은 서버 기본 규칙 값(protocol.md 4.2절 아래 설명). */
    return room_create(c, game_id, ROOM_VISIBILITY_PUBLIC, NULL, 0);
}

int room_join_by_id(conn *c, uint32_t room_id)
{
    room *r;
    HASH_FIND(hh, g_rooms, &room_id, sizeof room_id, r);
    /* 비공개 방 번호로 들어오면 코드를 건너뛰게 되므로 없는 방처럼 답한다. */
    if (r == NULL || r->visibility != ROOM_VISIBILITY_PUBLIC)
        return ERR_ROOM_NOT_FOUND;
    return join(r, c);
}

int room_join_by_code(conn *c, const uint8_t code[ROOM_CODE_LEN])
{
    room *r = NULL;
    bool chars_ok = true;
    for (int i = 0; i < ROOM_CODE_LEN; i++)
        chars_ok = chars_ok && room_code_char_ok(code[i]);
    /* 알파벳 밖 글자가 섞인 코드도 "틀린 코드"로 센다: 무작위 추측 횟수를 같은 규칙으로 막는다. */
    if (chars_ok)
        HASH_FIND(hh_code, g_by_code, code, ROOM_CODE_LEN, r);
    if (r == NULL) {
        c->code_fails++;
        return c->code_fails >= CODE_TRIES_MAX ? ERR_TOO_MANY_CODE_TRIES : ERR_BAD_CODE;
    }
    return join(r, c);
}

void room_leave(conn *c)
{
    room *r = c->room;
    if (r == NULL)
        return;
    int slot = c->slot;
    r->members[slot] = NULL;
    r->count--;
    c->room = NULL;
    c->slot = -1;
    uint8_t s = (uint8_t)slot;
    room_broadcast(r, NULL, MSG_PLAYER_LEFT, &s, 1);
    if (r->match != NULL)
        match_player_left(r, slot);
    /* 5.5절 3번: 누가 나가면 동의를 지우고 보통 대기 방으로 돌린다. */
    r->just_ended = false;
    r->votes = 0;
    if (r->count == 0) {
        destroy_room(r);
        return;
    }
    room_update_listing(r);
}

int room_send_list(conn *c, uint16_t game_id)
{
    size_t gi;
    if (games_find(game_id, &gi) == NULL)
        return ERR_UNKNOWN_GAME;
    uint8_t p[3 + 6 * ROOM_LIST_MAX];
    wire_put_u16(p, game_id);
    uint8_t count = 0;
    for (room *r = g_lobby[gi].head; r != NULL && count < ROOM_LIST_MAX; r = r->list_next) {
        uint8_t *e = p + 3 + 6 * count;
        wire_put_u32(e, r->id);
        e[4] = r->count;
        e[5] = r->game->max_players;
        count++;
    }
    p[2] = count;
    conn_send_frame(c, MSG_ROOM_LIST, p, 3 + 6 * (size_t)count);
    return 0;
}

void rooms_tick(uint64_t now)
{
    room *r, *tmp;
    HASH_ITER(hh, g_rooms, r, tmp) {
        if (r->match != NULL && r->game->tick_hz > 0)
            match_tick(r, now);
    }
}

int rooms_timeout_ms(uint64_t now)
{
    bool any = false;
    uint64_t soonest = 0;
    room *r, *tmp;
    HASH_ITER(hh, g_rooms, r, tmp) {
        if (r->match == NULL || r->game->tick_hz == 0)
            continue;
        uint64_t d = tick_clock_deadline(&r->clock);
        if (!any || d < soonest)
            soonest = d;
        any = true;
    }
    return any ? tick_ms_until(soonest, now) : -1;
}

void rooms_shutdown(void)
{
    room *r, *tmp;
    HASH_ITER(hh, g_rooms, r, tmp) {
        for (int i = 0; i < GAME_MAX_SLOTS; i++) {
            if (r->members[i] != NULL)
                r->members[i]->room = NULL;
        }
        destroy_room(r);
    }
    free(g_lobby);
    g_lobby = NULL;
}
