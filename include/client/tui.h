#ifndef WEREWOLF_CLIENT_TUI_H
#define WEREWOLF_CLIENT_TUI_H

#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

enum tui_log_kind {
    TUI_LOG_SYSTEM,
    TUI_LOG_SUCCESS,
    TUI_LOG_ERROR,
    TUI_LOG_VILLAGE,
    TUI_LOG_WOLVES,
    TUI_LOG_NIGHT,
    TUI_LOG_VOTE
};

struct tui_player_view {
    uint32_t id;
    const char *name;
    bool alive;
    bool alive_known;
    bool is_self;
    bool is_host;
};

struct tui_view {
    const char *username;
    uint32_t room_id;
    bool is_host;
    bool game_active;
    bool game_over;
    enum protocol_role role;
    enum protocol_phase phase;
    uint16_t duration_sec;
    size_t player_count;
    struct tui_player_view players[PROTOCOL_MAX_PLAYERS];
};

enum tui_key {
    TUI_KEY_NONE,
    TUI_KEY_CHARACTER,
    TUI_KEY_ENTER,
    TUI_KEY_BACKSPACE,
    TUI_KEY_EOF,
    TUI_KEY_RESIZE
};

bool tui_start(void);
void tui_stop(void);
bool tui_is_active(void);
void tui_log(enum tui_log_kind kind, const char *format, ...);
void tui_draw(const struct tui_view *view, const char *input);
enum tui_key tui_read_key(wchar_t *character);

#endif
