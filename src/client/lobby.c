#include "client/lobby.h"
#include "client/tui.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

enum {
    MAX_COMMAND = PROTOCOL_MAX_CHAT_BYTES + 16
};

struct player {
    uint32_t id;
    char name[PROTOCOL_MAX_USERNAME + 1];
};

struct room_view {
    uint32_t id;
    uint32_t host_id;
    uint32_t self_id;
    uint8_t count;
    struct player players[PROTOCOL_MAX_PLAYERS];
};

struct game_view {
    bool active;
    bool over;
    enum protocol_role role;
    enum protocol_phase phase;
    enum protocol_faction winner;
    uint16_t duration_sec;
    uint64_t deadline_ms;
    uint8_t player_count;
    uint32_t player_ids[PROTOCOL_MAX_PLAYERS];
    bool alive[PROTOCOL_MAX_PLAYERS];
    uint8_t wolf_count;
    uint32_t wolf_ids[PROTOCOL_MAX_PLAYERS];
};

struct client {
    const char *username;
    struct room_view room;
    struct game_view game;
    uint8_t input[PROTOCOL_MAX_FRAME];
    uint8_t output[PROTOCOL_MAX_FRAME];
    size_t received;
    size_t queued;
    size_t sent;
    uint32_t last_request_id;
    uint8_t pending_type;
    char command[MAX_COMMAND];
    size_t command_length;
    bool command_too_long;
};

static bool rich_terminal;

#define ANSI_RESET   "\x1b[0m"
#define ANSI_BOLD    "\x1b[1m"
#define ANSI_DIM     "\x1b[2m"
#define ANSI_RED     "\x1b[31m"
#define ANSI_GREEN   "\x1b[32m"
#define ANSI_YELLOW  "\x1b[33m"
#define ANSI_BLUE    "\x1b[34m"
#define ANSI_MAGENTA "\x1b[35m"
#define ANSI_CYAN    "\x1b[36m"

static void print_name(const char *name);
static void refresh_tui(const struct client *client);

static void show_message(enum tui_log_kind kind, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    if (tui_is_active()) {
        char message[PROTOCOL_MAX_CHAT_BYTES + 160];
        vsnprintf(message, sizeof message, format, arguments);
        tui_log(kind, "%s", message);
    } else {
        vprintf(format, arguments);
        putchar('\n');
    }
    va_end(arguments);
}

static void set_style(const char *style)
{
    if (rich_terminal) {
        fputs(style, stdout);
    }
}

static void print_rule(const char *title, const char *color)
{
    if (!rich_terminal) {
        return;
    }
    set_style(color);
    set_style(ANSI_BOLD);
    printf("\n==================== %s ====================\n", title);
    set_style(ANSI_RESET);
}

static const char *prompt_state(const struct client *client)
{
    if (client->room.id == 0) {
        return "LOBBY";
    }
    if (!client->game.active) {
        return client->room.self_id == client->room.host_id ? "ROOM/HOST" : "ROOM";
    }
    if (client->game.over) {
        return "GAME OVER";
    }
    switch (client->game.phase) {
    case PROTOCOL_PHASE_NIGHT: return "NIGHT";
    case PROTOCOL_PHASE_DAY_DISCUSS: return "DAY/CHAT";
    case PROTOCOL_PHASE_DAY_VOTE: return "DAY/VOTE";
    }
    return "GAME";
}

static const char *prompt_color(const struct client *client)
{
    if (client->game.active && !client->game.over) {
        if (client->game.phase == PROTOCOL_PHASE_NIGHT) return ANSI_MAGENTA;
        if (client->game.phase == PROTOCOL_PHASE_DAY_VOTE) return ANSI_YELLOW;
        return ANSI_CYAN;
    }
    return client->room.id == 0 ? ANSI_BLUE : ANSI_GREEN;
}

static void prompt(const struct client *client)
{
    if (tui_is_active()) {
        refresh_tui(client);
        return;
    }
    if (rich_terminal) {
        set_style(ANSI_BOLD);
        set_style(prompt_color(client));
        print_name(client->username);
        printf(" [%s]", prompt_state(client));
        set_style(ANSI_RESET);
    }
    fputs("> ", stdout);
    fflush(stdout);
}

static void print_commands(void)
{
    if (tui_is_active()) {
        tui_log(TUI_LOG_SYSTEM, "Lobby: list | create <name> <5-12> | join <room_id>");
        tui_log(TUI_LOG_SYSTEM, "Room: invite <username> | kick <player_id> | leave | start");
        tui_log(TUI_LOG_SYSTEM, "Game: act <player_id> | chat <text> | vote <player_id|0>");
        tui_log(TUI_LOG_SYSTEM, "Info: status | help");
        return;
    }
    if (rich_terminal) {
        print_rule("COMMANDS", ANSI_BLUE);
        puts("  Lobby : list | create <name> <5-12> | join <room_id>");
        puts("          invite <username> | kick <player_id> | leave | start");
        puts("  Game  : act <player_id> | chat <text> | vote <player_id|0>");
        puts("  Info  : status | help");
        return;
    }
    puts("Commands: list | create <name> <5-12> | join <room_id> | "
         "invite <username> | kick <player_id> | leave | start | "
         "act <player_id> | chat <text> | vote <player_id|0> | status | help");
}

static bool parse_number(const char *text, uint32_t *value)
{
    if (text == NULL || text[0] < '0' || text[0] > '9') {
        return false;
    }
    char *end;
    errno = 0;
    unsigned long number = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || number == 0 || number > UINT32_MAX) {
        return false;
    }
    *value = (uint32_t)number;
    return true;
}

static bool parse_vote_target(const char *text, uint32_t *value)
{
    if (text == NULL || text[0] < '0' || text[0] > '9') {
        return false;
    }
    char *end;
    errno = 0;
    unsigned long number = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || number > UINT32_MAX) {
        return false;
    }
    *value = (uint32_t)number;
    return true;
}

static bool queue_request(struct client *client, uint8_t type,
                          const uint8_t *body, size_t length)
{
    if (client->pending_type != 0 || length > PROTOCOL_MAX_BODY) {
        return false;
    }
    if (length != 0 && body != client->output + PROTOCOL_HEADER_SIZE) {
        memmove(client->output + PROTOCOL_HEADER_SIZE, body, length);
    }
    if (++client->last_request_id == 0) {
        ++client->last_request_id;
    }
    struct message_header header = {
        .type = type,
        .length = (uint16_t)length,
        .request_id = client->last_request_id
    };
    protocol_encode_header(client->output, &header);
    client->pending_type = type;
    client->queued = PROTOCOL_HEADER_SIZE + length;
    client->sent = 0;
    return true;
}

static bool game_self_alive(const struct client *client)
{
    for (size_t i = 0; i < client->game.player_count; ++i) {
        if (client->game.player_ids[i] == client->room.self_id) {
            return client->game.alive[i];
        }
    }
    return false;
}

static void print_game_status(const struct client *client);

// Đổi lệnh terminal thành yêu cầu có request_id riêng.
static void queue_command(struct client *client)
{
    uint8_t *body = client->output + PROTOCOL_HEADER_SIZE;
    uint8_t type;
    size_t length = 0;
    char *command = client->command;
    while (*command != '\0' && isspace((unsigned char)*command)) {
        ++command;
    }
    if (*command == '\0') {
        prompt(client);
        return;
    }
    char *argument = command;
    while (*argument != '\0' && !isspace((unsigned char)*argument)) {
        ++argument;
    }
    if (*argument != '\0') {
        *argument++ = '\0';
        while (*argument != '\0' && isspace((unsigned char)*argument)) {
            ++argument;
        }
    }

    if (strcmp(command, "help") == 0 && argument[0] == '\0') {
        print_commands();
        prompt(client);
        return;
    }
    if (strcmp(command, "status") == 0 && argument[0] == '\0') {
        print_game_status(client);
        prompt(client);
        return;
    }

    if (client->game.active && !client->game.over) {
        if (strcmp(command, "chat") == 0 && argument[0] != '\0') {
            if (!game_self_alive(client)) {
                show_message(TUI_LOG_ERROR, "Dead players can only observe");
                prompt(client);
                return;
            }
            uint8_t scope;
            if (client->game.phase == PROTOCOL_PHASE_DAY_DISCUSS) {
                scope = PROTOCOL_CHAT_VILLAGE;
            } else if (client->game.phase == PROTOCOL_PHASE_NIGHT &&
                       client->game.role == PROTOCOL_ROLE_WEREWOLF) {
                scope = PROTOCOL_CHAT_WEREWOLF;
            } else {
                show_message(TUI_LOG_ERROR, "Chat is not available in this phase");
                prompt(client);
                return;
            }
            size_t text_length = strlen(argument);
            if (text_length > PROTOCOL_MAX_CHAT_BYTES ||
                memchr(argument, '\0', text_length) != NULL) {
                show_message(TUI_LOG_ERROR, "Chat message is too long or invalid");
                prompt(client);
                return;
            }
            body[0] = scope;
            memcpy(body + 1, argument, text_length);
            if (!queue_request(client, MSG_CHAT_MSG, body, 1 + text_length)) {
                show_message(TUI_LOG_ERROR, "Could not queue chat message");
                prompt(client);
            }
            return;
        }
        if (strcmp(command, "act") == 0) {
            uint32_t target_id;
            if (!game_self_alive(client)) {
                show_message(TUI_LOG_ERROR, "Dead players can only observe");
            } else if (client->game.phase != PROTOCOL_PHASE_NIGHT) {
                show_message(TUI_LOG_ERROR, "Night actions are only available at night");
            } else if (!parse_number(argument, &target_id)) {
                show_message(TUI_LOG_ERROR, "Usage: act <player_id>");
            } else {
                protocol_put_u32(body, target_id);
                if (!queue_request(client, MSG_NIGHT_ACTION_REQ, body, 4)) {
                    show_message(TUI_LOG_ERROR, "Could not queue night action");
                }
            }
            if (client->pending_type == 0) {
                prompt(client);
            }
            return;
        }
        if (strcmp(command, "vote") == 0) {
            uint32_t target_id;
            if (!game_self_alive(client)) {
                show_message(TUI_LOG_ERROR, "Dead players can only observe");
            } else if (client->game.phase != PROTOCOL_PHASE_DAY_VOTE) {
                show_message(TUI_LOG_ERROR, "Voting is only available during the vote phase");
            } else if (!parse_vote_target(argument, &target_id)) {
                show_message(TUI_LOG_ERROR, "Usage: vote <player_id|0>");
            } else {
                protocol_put_u32(body, target_id);
                if (!queue_request(client, MSG_DAY_VOTE_REQ, body, 4)) {
                    show_message(TUI_LOG_ERROR, "Could not queue vote");
                }
            }
            if (client->pending_type == 0) {
                prompt(client);
            }
            return;
        }
        show_message(TUI_LOG_ERROR,
                     "Only game actions, chat, vote, status, and help are available during a game");
        prompt(client);
        return;
    }

    uint32_t number;
    char *argument_copy = argument;
    char *first = strtok(argument_copy, " \t\r");
    char *second = strtok(NULL, " \t\r");
    if (strcmp(command, "list") == 0 && first == NULL) {
        type = MSG_ROOM_LIST_REQ;
    } else if (strcmp(command, "create") == 0 && first != NULL &&
               strlen(first) <= PROTOCOL_MAX_NAME &&
               parse_number(second, &number) &&
               number >= PROTOCOL_MIN_PLAYERS && number <= PROTOCOL_MAX_PLAYERS &&
               strtok(NULL, " \t\r") == NULL) {
        type = MSG_CREATE_ROOM_REQ;
        length = protocol_put_name(body, first);
        body[length++] = (uint8_t)number;
    } else if (strcmp(command, "join") == 0 && parse_number(first, &number) &&
               second == NULL) {
        type = MSG_JOIN_ROOM_REQ;
        protocol_put_u32(body, number);
        length = 4;
    } else if (strcmp(command, "invite") == 0 && first != NULL &&
               strlen(first) <= PROTOCOL_MAX_USERNAME &&
               second == NULL) {
        type = MSG_INVITE_REQ;
        length = protocol_put_name(body, first);
    } else if (strcmp(command, "kick") == 0 && parse_number(first, &number) &&
               second == NULL) {
        type = MSG_KICK_REQ;
        protocol_put_u32(body, number);
        length = 4;
    } else if (strcmp(command, "leave") == 0 && first == NULL) {
        type = MSG_LEAVE_ROOM_REQ;
    } else if (strcmp(command, "start") == 0 && first == NULL) {
        if (client->room.id == 0) {
            show_message(TUI_LOG_ERROR, "Join or create a room first");
            prompt(client);
            return;
        }
        if (client->room.self_id != client->room.host_id) {
            show_message(TUI_LOG_ERROR, "Only the host can start the game");
            prompt(client);
            return;
        }
        type = MSG_START_GAME_REQ;
    } else {
        print_commands();
        prompt(client);
        return;
    }

    if (!queue_request(client, type, body, length)) {
        show_message(TUI_LOG_ERROR, "Could not queue request");
        prompt(client);
    }
}

static void print_name(const char *name)
{
    mbstate_t state = {0};
    size_t remaining = strlen(name);
    while (remaining != 0) {
        wchar_t character;
        size_t width = mbrtowc(&character, name, remaining, &state);
        if (width == (size_t)-1 || width == (size_t)-2) {
            state = (mbstate_t){0};
            width = 1;
        } else if (iswprint(character) && character != L'\\') {
            fwrite(name, 1, width, stdout);
            name += width;
            remaining -= width;
            continue;
        }
        for (size_t i = 0; i < width; ++i) {
            printf("\\x%02x", (unsigned)(unsigned char)name[i]);
        }
        name += width;
        remaining -= width;
    }
}

static bool print_room_list(const uint8_t *body, size_t length)
{
    if (length < 2) {
        return false;
    }
    uint16_t count = protocol_get_u16(body);
    size_t offset = 2;
    if (tui_is_active()) {
        tui_log(TUI_LOG_SYSTEM, "Rooms: %u", (unsigned)count);
    } else {
        printf("Rooms: %u\n", (unsigned)count);
    }
    for (size_t i = 0; i < count; ++i) {
        if (length - offset < 4) {
            return false;
        }
        uint32_t id = protocol_get_u32(body + offset);
        offset += 4;
        char name[PROTOCOL_MAX_NAME + 1];
        if (!protocol_get_name(body, length, &offset, name) ||
            length - offset < 2) {
            return false;
        }
        unsigned players = body[offset++];
        unsigned capacity = body[offset++];
        if (id == 0 || players == 0 || players > capacity ||
            capacity < PROTOCOL_MIN_PLAYERS || capacity > PROTOCOL_MAX_PLAYERS) {
            return false;
        }
        if (tui_is_active()) {
            tui_log(TUI_LOG_SYSTEM, "Room #%" PRIu32 "  %s  (%u/%u)",
                    id, name, players, capacity);
        } else {
            printf("  room_id=%" PRIu32 " ", id);
            print_name(name);
            printf(" (%u/%u)\n", players, capacity);
        }
    }
    return offset == length;
}

static bool update_room(struct client *client, const uint8_t *body, size_t length)
{
    if (length < 9 || body[8] == 0 || body[8] > PROTOCOL_MAX_PLAYERS) {
        return false;
    }
    struct room_view room = {
        .id = protocol_get_u32(body),
        .host_id = protocol_get_u32(body + 4),
        .count = body[8]
    };
    size_t offset = 9;
    bool has_host = false;
    for (size_t i = 0; i < room.count; ++i) {
        if (length - offset < 4) {
            return false;
        }
        struct player *player = &room.players[i];
        player->id = protocol_get_u32(body + offset);
        offset += 4;
        if (player->id == 0 || !protocol_get_name(body, length, &offset, player->name)) {
            return false;
        }
        if (strcmp(player->name, client->username) == 0) {
            room.self_id = player->id;
        }
        if (player->id == room.host_id) {
            has_host = true;
        }
    }
    if (offset != length || room.id == 0 || !has_host || room.self_id == 0) {
        return false;
    }

    // Chỉ cập nhật trạng thái sau khi kiểm tra đủ dữ liệu.
    client->room = room;
    if (tui_is_active()) {
        tui_log(TUI_LOG_SYSTEM, "Room updated: #%" PRIu32 " with %u player(s)%s",
                room.id, (unsigned)room.count,
                room.self_id == room.host_id ? "; you are the host" : "");
        refresh_tui(client);
        return true;
    }
    print_rule("ROOM", ANSI_GREEN);
    set_style(ANSI_CYAN);
    printf("%sRoom update: room_id=%" PRIu32 " host_id=%" PRIu32 " self_id=%" PRIu32 "\n",
           rich_terminal ? "" : "\n", room.id, room.host_id, room.self_id);
    set_style(ANSI_RESET);
    for (size_t i = 0; i < client->room.count; ++i) {
        const struct player *player = &client->room.players[i];
        printf("  %" PRIu32 " ", player->id);
        print_name(player->name);
        if (rich_terminal && player->id == room.host_id) {
            set_style(ANSI_YELLOW);
            fputs(" [HOST]", stdout);
            set_style(ANSI_RESET);
        }
        if (rich_terminal && player->id == room.self_id) {
            set_style(ANSI_CYAN);
            fputs(" [YOU]", stdout);
            set_style(ANSI_RESET);
        }
        putchar('\n');
    }
    set_style(room.self_id == room.host_id ? ANSI_YELLOW : ANSI_GREEN);
    puts(room.self_id == room.host_id ? "You are the host" : "You are a member");
    set_style(ANSI_RESET);
    return true;
}

static int game_player_index(const struct game_view *game, uint32_t player_id)
{
    for (size_t i = 0; i < game->player_count; ++i) {
        if (game->player_ids[i] == player_id) {
            return (int)i;
        }
    }
    return -1;
}

static const char *room_player_name(const struct client *client,
                                   uint32_t player_id)
{
    for (size_t i = 0; i < client->room.count; ++i) {
        if (client->room.players[i].id == player_id) {
            return client->room.players[i].name;
        }
    }
    return NULL;
}

static void refresh_tui(const struct client *client)
{
    if (!tui_is_active()) return;
    struct tui_view view = {
        .username = client->username,
        .room_id = client->room.id,
        .is_host = client->room.id != 0 &&
                   client->room.self_id == client->room.host_id,
        .game_active = client->game.active,
        .game_over = client->game.over,
        .role = client->game.role,
        .phase = client->game.phase,
        .duration_sec = client->game.duration_sec
    };
    if (client->game.active) {
        view.player_count = client->game.player_count;
        for (size_t i = 0; i < view.player_count; ++i) {
            uint32_t id = client->game.player_ids[i];
            const char *name = room_player_name(client, id);
            view.players[i] = (struct tui_player_view){
                .id = id,
                .name = name == NULL ? "(left room)" : name,
                .alive = client->game.alive[i],
                .alive_known = true,
                .is_self = id == client->room.self_id,
                .is_host = id == client->room.host_id
            };
        }
    } else {
        view.player_count = client->room.count;
        for (size_t i = 0; i < view.player_count; ++i) {
            const struct player *player = &client->room.players[i];
            view.players[i] = (struct tui_player_view){
                .id = player->id,
                .name = player->name,
                .is_self = player->id == client->room.self_id,
                .is_host = player->id == client->room.host_id
            };
        }
    }
    tui_draw(&view, client->command);
}

static void print_player_id(const struct client *client, uint32_t player_id)
{
    const char *name = room_player_name(client, player_id);
    if (name != NULL) {
        print_name(name);
    } else {
        printf("player #%" PRIu32, player_id);
    }
}

static void start_game_view(struct client *client)
{
    client->game = (struct game_view){.active = true};
    client->game.player_count = client->room.count;
    for (size_t i = 0; i < client->room.count; ++i) {
        client->game.player_ids[i] = client->room.players[i].id;
        client->game.alive[i] = true;
    }
}

static const char *role_name(enum protocol_role role)
{
    switch (role) {
    case PROTOCOL_ROLE_WEREWOLF: return "Werewolf";
    case PROTOCOL_ROLE_VILLAGER: return "Villager";
    case PROTOCOL_ROLE_SEER: return "Seer";
    case PROTOCOL_ROLE_GUARD: return "Guard";
    }
    return "Unknown";
}

static const char *phase_name(enum protocol_phase phase)
{
    switch (phase) {
    case PROTOCOL_PHASE_NIGHT: return "Night";
    case PROTOCOL_PHASE_DAY_DISCUSS: return "Day discussion";
    case PROTOCOL_PHASE_DAY_VOTE: return "Day vote";
    }
    return "Unknown";
}

static void print_game_status(const struct client *client)
{
    if (tui_is_active()) {
        tui_log(TUI_LOG_SYSTEM, "Status refreshed");
        refresh_tui(client);
        return;
    }
    if (rich_terminal) {
        print_rule("STATUS", ANSI_CYAN);
        printf("  Player : ");
        set_style(ANSI_BOLD);
        print_name(client->username);
        set_style(ANSI_RESET);
        printf("\n  Room   : %s", client->room.id == 0 ? "Lobby" : "Joined");
        if (client->room.id != 0) {
            printf(" #%" PRIu32 "%s", client->room.id,
                   client->room.self_id == client->room.host_id ? " (Host)" : "");
        }
        putchar('\n');
        if (!client->game.active) {
            puts("  Game   : Waiting");
            return;
        }
        if (client->game.over) {
            puts("  Game   : Over; waiting for the next game");
            return;
        }
        printf("  Role   : %s\n", role_name(client->game.role));
        printf("  Phase  : %s (%u seconds)\n", phase_name(client->game.phase),
               (unsigned)client->game.duration_sec);
        puts("\n  ID   PLAYER                         STATE");
        puts("  ---- ------------------------------ --------");
        for (size_t i = 0; i < client->game.player_count; ++i) {
            uint32_t id = client->game.player_ids[i];
            printf("  %-4" PRIu32 " ", id);
            const char *name = room_player_name(client, id);
            if (name != NULL) {
                print_name(name);
            } else {
                printf("player #%" PRIu32, id);
            }
            if (id == client->room.self_id) fputs(" [YOU]", stdout);
            if (id == client->room.host_id) fputs(" [HOST]", stdout);
            fputs("\t", stdout);
            set_style(client->game.alive[i] ? ANSI_GREEN : ANSI_DIM);
            puts(client->game.alive[i] ? "ALIVE" : "DEAD");
            set_style(ANSI_RESET);
        }
        return;
    }
    if (!client->game.active) {
        puts(client->room.id == 0 ? "You are in the lobby" : "You are in a room");
        return;
    }
    if (client->game.over) {
        puts("Game over; observing room events");
        return;
    }
    printf("Role: %s | Phase: %s | Duration: %u seconds\n",
           role_name(client->game.role), phase_name(client->game.phase),
           (unsigned)client->game.duration_sec);
    fputs("Players alive:", stdout);
    for (size_t i = 0; i < client->game.player_count; ++i) {
        if (client->game.alive[i]) {
            fputs(" ", stdout);
            print_player_id(client, client->game.player_ids[i]);
        }
    }
    putchar('\n');
}

static bool game_push_header_valid(const struct message_header *header)
{
    return header->request_id == 0 && header->status == ERR_SUCCESS;
}

static bool game_push_type(uint8_t type)
{
    switch (type) {
    case MSG_ROLE_ASSIGN:
    case MSG_WOLF_TEAM_INFO:
    case MSG_PHASE_CHANGE:
    case MSG_SEER_RESULT:
    case MSG_WOLF_TARGET_UPDATE:
    case MSG_NIGHT_RESULT:
    case MSG_DAY_RESULT:
    case MSG_GAME_OVER:
    case MSG_CHAT_NOTIF:
        return true;
    }
    return false;
}

static bool handle_game_push(struct client *client,
                             const struct message_header *header)
{
    const uint8_t *body = client->input + PROTOCOL_HEADER_SIZE;
    if (!game_push_header_valid(header)) {
        return false;
    }

    switch (header->type) {
    case MSG_ROLE_ASSIGN:
        if (header->length != 1 || body[0] < PROTOCOL_ROLE_WEREWOLF ||
            body[0] > PROTOCOL_ROLE_GUARD || client->room.id == 0) {
            return false;
        }
        if (!client->game.active || client->game.over) {
            start_game_view(client);
        }
        client->game.role = (enum protocol_role)body[0];
        if (tui_is_active()) {
            tui_log(client->game.role == PROTOCOL_ROLE_WEREWOLF
                        ? TUI_LOG_WOLVES : TUI_LOG_SUCCESS,
                    "Game started. Your role is %s",
                    role_name(client->game.role));
        }
        print_rule("GAME STARTED", ANSI_YELLOW);
        set_style(client->game.role == PROTOCOL_ROLE_WEREWOLF ? ANSI_RED : ANSI_CYAN);
        printf("%sGame started. Your role is %s\n", rich_terminal ? "" : "\n",
               role_name(client->game.role));
        set_style(ANSI_RESET);
        return true;
    case MSG_WOLF_TEAM_INFO: {
        if (header->length < 1 || client->game.role != PROTOCOL_ROLE_WEREWOLF) {
            return false;
        }
        uint8_t count = body[0];
        if (count == 0 || count > PROTOCOL_MAX_PLAYERS ||
            header->length != 1 + (size_t)count * 4) {
            return false;
        }
        uint32_t ids[PROTOCOL_MAX_PLAYERS];
        bool has_self = false;
        for (size_t i = 0; i < count; ++i) {
            ids[i] = protocol_get_u32(body + 1 + i * 4);
            if (game_player_index(&client->game, ids[i]) < 0) {
                return false;
            }
            for (size_t j = 0; j < i; ++j) {
                if (ids[i] == ids[j]) {
                    return false;
                }
            }
            has_self = has_self || ids[i] == client->room.self_id;
        }
        if (!has_self) {
            return false;
        }
        client->game.wolf_count = count;
        memcpy(client->game.wolf_ids, ids, count * sizeof ids[0]);
        if (tui_is_active()) {
            for (size_t i = 0; i < count; ++i) {
                const char *name = room_player_name(client, ids[i]);
                tui_log(TUI_LOG_WOLVES, "Wolf teammate: %s (#%" PRIu32 ")",
                        name == NULL ? "unknown" : name, ids[i]);
            }
        }
        set_style(ANSI_RED);
        fputs("Wolf team:", stdout);
        for (size_t i = 0; i < count; ++i) {
            fputs(" ", stdout);
            print_player_id(client, ids[i]);
        }
        putchar('\n');
        set_style(ANSI_RESET);
        return true;
    }
    case MSG_PHASE_CHANGE: {
        if (header->length != 11 || body[0] < PROTOCOL_PHASE_NIGHT ||
            body[0] > PROTOCOL_PHASE_DAY_VOTE) {
            return false;
        }
        uint16_t duration = protocol_get_u16(body + 1);
        if (duration == 0) {
            return false;
        }
        if (!client->game.active || client->game.over) {
            start_game_view(client);
        }
        client->game.phase = (enum protocol_phase)body[0];
        client->game.duration_sec = duration;
        client->game.deadline_ms = protocol_get_u64(body + 3);
        if (tui_is_active()) {
            tui_log(client->game.phase == PROTOCOL_PHASE_NIGHT
                        ? TUI_LOG_NIGHT : client->game.phase == PROTOCOL_PHASE_DAY_VOTE
                        ? TUI_LOG_VOTE : TUI_LOG_SYSTEM,
                    "Phase changed to %s (%u seconds)",
                    phase_name(client->game.phase), (unsigned)duration);
        }
        const char *phase_color = client->game.phase == PROTOCOL_PHASE_NIGHT
            ? ANSI_MAGENTA : client->game.phase == PROTOCOL_PHASE_DAY_VOTE
            ? ANSI_YELLOW : ANSI_CYAN;
        print_rule(phase_name(client->game.phase), phase_color);
        set_style(phase_color);
        printf("%sPhase: %s (%u seconds)\n", rich_terminal ? "" : "\n",
               phase_name(client->game.phase), (unsigned)duration);
        set_style(ANSI_RESET);
        if (client->game.phase == PROTOCOL_PHASE_NIGHT) {
            puts("Use act <player_id> if your role has a night action");
            if (client->game.role == PROTOCOL_ROLE_WEREWOLF) {
                puts("Wolves may use chat <text> for private night chat");
            }
        } else if (client->game.phase == PROTOCOL_PHASE_DAY_DISCUSS) {
            puts("Living players may use chat <text>");
        } else {
            puts("Living players may use vote <player_id|0>");
        }
        return true;
    }
    case MSG_SEER_RESULT: {
        if (header->length != 5 || client->game.role != PROTOCOL_ROLE_SEER ||
            body[4] > 1 || game_player_index(&client->game,
                                             protocol_get_u32(body)) < 0) {
            return false;
        }
        if (tui_is_active()) {
            uint32_t id = protocol_get_u32(body);
            const char *name = room_player_name(client, id);
            tui_log(TUI_LOG_NIGHT, "Seer result: %s (#%" PRIu32 ") is %sa werewolf",
                    name == NULL ? "unknown" : name, id, body[4] ? "" : "not ");
        }
        set_style(ANSI_MAGENTA);
        if (rich_terminal) fputs("[Seer] ", stdout);
        fputs("Seer result: ", stdout);
        print_player_id(client, protocol_get_u32(body));
        puts(body[4] ? " is a werewolf" : " is not a werewolf");
        set_style(ANSI_RESET);
        return true;
    }
    case MSG_WOLF_TARGET_UPDATE: {
        if (header->length != 8 || client->game.role != PROTOCOL_ROLE_WEREWOLF) {
            return false;
        }
        uint32_t wolf_id = protocol_get_u32(body);
        uint32_t target_id = protocol_get_u32(body + 4);
        if (game_player_index(&client->game, wolf_id) < 0 ||
            game_player_index(&client->game, target_id) < 0) {
            return false;
        }
        if (tui_is_active()) {
            const char *wolf_name = room_player_name(client, wolf_id);
            const char *target_name = room_player_name(client, target_id);
            tui_log(TUI_LOG_WOLVES, "%s chose %s as wolf target",
                    wolf_name == NULL ? "A wolf" : wolf_name,
                    target_name == NULL ? "unknown" : target_name);
        }
        set_style(ANSI_RED);
        if (rich_terminal) fputs("[Wolves] ", stdout);
        fputs("Wolf target: ", stdout);
        print_player_id(client, wolf_id);
        fputs(" chose ", stdout);
        print_player_id(client, target_id);
        putchar('\n');
        set_style(ANSI_RESET);
        return true;
    }
    case MSG_NIGHT_RESULT:
    case MSG_DAY_RESULT: {
        if (header->length != 4) {
            return false;
        }
        uint32_t player_id = protocol_get_u32(body);
        if (player_id != 0) {
            int index = game_player_index(&client->game, player_id);
            if (index < 0 || !client->game.alive[index]) {
                return false;
            }
            client->game.alive[index] = false;
        }
        if (header->type == MSG_NIGHT_RESULT) {
            if (tui_is_active()) {
                const char *name = room_player_name(client, player_id);
                if (player_id == 0) {
                    tui_log(TUI_LOG_NIGHT, "Nobody died tonight");
                } else {
                    tui_log(TUI_LOG_NIGHT, "%s (#%" PRIu32 ") died tonight",
                            name == NULL ? "A player" : name, player_id);
                }
            }
            set_style(ANSI_MAGENTA);
            if (rich_terminal) fputs("[Night] ", stdout);
            if (player_id == 0) {
                puts("Night result: nobody died");
            } else {
                fputs("Night result: ", stdout);
                print_player_id(client, player_id);
                puts(" died");
            }
        } else if (player_id == 0) {
            if (tui_is_active())
                tui_log(TUI_LOG_VOTE, "Nobody was executed");
            set_style(ANSI_YELLOW);
            if (rich_terminal) fputs("[Vote] ", stdout);
            puts("Day result: nobody was executed");
        } else {
            if (tui_is_active()) {
                const char *name = room_player_name(client, player_id);
                tui_log(TUI_LOG_VOTE, "%s (#%" PRIu32 ") was executed",
                        name == NULL ? "A player" : name, player_id);
            }
            set_style(ANSI_YELLOW);
            if (rich_terminal) fputs("[Vote] ", stdout);
            fputs("Day result: ", stdout);
            print_player_id(client, player_id);
            puts(" was executed");
        }
        set_style(ANSI_RESET);
        return true;
    }
    case MSG_CHAT_NOTIF: {
        if (header->length < 6 || header->length > 5 + PROTOCOL_MAX_CHAT_BYTES ||
            body[0] > PROTOCOL_CHAT_WEREWOLF ||
            memchr(body + 5, '\0', header->length - 5) != NULL) {
            return false;
        }
        uint32_t from_id = protocol_get_u32(body + 1);
        if (from_id == 0 || game_player_index(&client->game, from_id) < 0 ||
            (body[0] == PROTOCOL_CHAT_WEREWOLF &&
             client->game.role != PROTOCOL_ROLE_WEREWOLF)) {
            return false;
        }
        set_style(body[0] == PROTOCOL_CHAT_WEREWOLF ? ANSI_RED : ANSI_GREEN);
        fputs(body[0] == PROTOCOL_CHAT_WEREWOLF ? "[Wolves] " : "[Village] ",
              stdout);
        print_player_id(client, from_id);
        fputs(": ", stdout);
        char message[PROTOCOL_MAX_CHAT_BYTES + 1];
        size_t message_length = header->length - 5;
        memcpy(message, body + 5, message_length);
        message[message_length] = '\0';
        if (tui_is_active()) {
            const char *name = room_player_name(client, from_id);
            tui_log(body[0] == PROTOCOL_CHAT_WEREWOLF
                        ? TUI_LOG_WOLVES : TUI_LOG_VILLAGE,
                    "%s: %s", name == NULL ? "unknown" : name, message);
        }
        print_name(message);
        putchar('\n');
        set_style(ANSI_RESET);
        return true;
    }
    case MSG_GAME_OVER: {
        if (header->length < 1 || header->length > 1 + PROTOCOL_MAX_SUMMARY_BYTES ||
            body[0] > PROTOCOL_FACTION_WEREWOLF ||
            memchr(body + 1, '\0', header->length - 1) != NULL) {
            return false;
        }
        client->game.active = true;
        client->game.over = true;
        client->game.winner = (enum protocol_faction)body[0];
        client->game.phase = 0;
        if (tui_is_active()) {
            tui_log(body[0] == PROTOCOL_FACTION_WEREWOLF
                        ? TUI_LOG_WOLVES : TUI_LOG_SUCCESS,
                    "%s", body[0] == PROTOCOL_FACTION_VILLAGE
                        ? "Game over: village wins"
                        : body[0] == PROTOCOL_FACTION_WEREWOLF
                        ? "Game over: werewolves win"
                        : "Game over: game cancelled");
        }
        print_rule("GAME OVER", ANSI_YELLOW);
        set_style(body[0] == PROTOCOL_FACTION_WEREWOLF ? ANSI_RED : ANSI_GREEN);
        if (body[0] == PROTOCOL_FACTION_VILLAGE) {
            puts(rich_terminal ? "Game over: village wins" : "\nGame over: village wins");
        } else if (body[0] == PROTOCOL_FACTION_WEREWOLF) {
            puts(rich_terminal ? "Game over: werewolves win" : "\nGame over: werewolves win");
        } else {
            puts(rich_terminal ? "Game over: game cancelled" : "\nGame over: game cancelled");
        }
        if (header->length > 1) {
            char summary[PROTOCOL_MAX_SUMMARY_BYTES + 1];
            size_t summary_length = header->length - 1;
            memcpy(summary, body + 1, summary_length);
            summary[summary_length] = '\0';
            print_name(summary);
            putchar('\n');
        }
        puts("You can keep watching room events");
        set_style(ANSI_RESET);
        return true;
    }
    default:
        return false;
    }
}

static void print_request_error(uint8_t status)
{
    const char *name = "Unknown error";
    switch (status) {
    case ERR_UNAUTHENTICATED: name = "ERR_UNAUTHENTICATED"; break;
    case ERR_UNAUTHORIZED: name = "ERR_UNAUTHORIZED"; break;
    case ERR_WRONG_PHASE: name = "ERR_WRONG_PHASE"; break;
    case ERR_INVALID_TARGET: name = "ERR_INVALID_TARGET"; break;
    case ERR_ROOM_FULL: name = "ERR_ROOM_FULL"; break;
    case ERR_INVALID_PAYLOAD: name = "ERR_INVALID_PAYLOAD"; break;
    case ERR_ROOM_NOT_FOUND: name = "ERR_ROOM_NOT_FOUND"; break;
    case ERR_GAME_RUNNING: name = "ERR_GAME_RUNNING"; break;
    case ERR_NOT_ENOUGH_PLAYERS: name = "ERR_NOT_ENOUGH_PLAYERS"; break;
    case ERR_RATE_LIMITED: name = "ERR_RATE_LIMITED"; break;
    case ERR_INTERNAL: name = "ERR_INTERNAL"; break;
    }
    if (tui_is_active()) {
        tui_log(TUI_LOG_ERROR, "%s (0x%02X)", name, (unsigned)status);
        return;
    }
    set_style(ANSI_RED);
    if (rich_terminal) fputs("[Error] ", stdout);
    printf("%s (0x%02X)\n", name, (unsigned)status);
    set_style(ANSI_RESET);
}

// Phân biệt cập nhật đẩy với phản hồi lệnh đang chờ.
static bool handle_message(struct client *client, const struct message_header *header)
{
    const uint8_t *body = client->input + PROTOCOL_HEADER_SIZE;
    if (header->type == MSG_ROOM_UPDATE) {
        return header->request_id == 0 && header->status == ERR_SUCCESS &&
               update_room(client, body, header->length);
    }
    if (header->type == MSG_INVITE_NOTIF) {
        char host[PROTOCOL_MAX_NAME + 1];
        size_t offset = 4;
        if (header->request_id != 0 || header->status != ERR_SUCCESS ||
            header->length < 5 || protocol_get_u32(body) == 0 ||
            !protocol_get_name(body, header->length, &offset, host) ||
            offset != header->length) {
            return false;
        }
        if (tui_is_active()) {
            tui_log(TUI_LOG_SYSTEM, "Invitation from %s: join room #%" PRIu32,
                    host, protocol_get_u32(body));
        }
        set_style(ANSI_CYAN);
        fputs("\nInvitation from ", stdout);
        print_name(host);
        printf(": join %" PRIu32 "\n", protocol_get_u32(body));
        set_style(ANSI_RESET);
        return true;
    }
    if (header->type == MSG_KICK_NOTIF) {
        if (header->request_id != 0 || header->status != ERR_SUCCESS ||
            header->length != 1 || body[0] != KICK_REASON_HOST) {
            return false;
        }
        client->room = (struct room_view){0};
        if (tui_is_active()) {
            tui_log(TUI_LOG_ERROR, "Kicked by host; returned to lobby");
        }
        set_style(ANSI_RED);
        puts("\nKicked by host; returned to lobby");
        set_style(ANSI_RESET);
        return true;
    }
    if (game_push_type(header->type)) {
        return handle_game_push(client, header);
    }
    if (client->pending_type == 0 || header->request_id != client->last_request_id) {
        return false;
    }
    if (header->type == MSG_ROOM_LIST_RESP && client->pending_type == MSG_ROOM_LIST_REQ &&
        header->status == ERR_SUCCESS) {
        if (!print_room_list(body, header->length)) {
            return false;
        }
    } else if (header->type == MSG_RESPONSE && header->length == 0) {
        if (header->status != ERR_SUCCESS) {
            print_request_error(header->status);
        } else if (client->pending_type == MSG_ROOM_LIST_REQ) {
            return false;
        } else {
            if (tui_is_active()) {
                const char *message = NULL;
                switch (client->pending_type) {
                case MSG_CREATE_ROOM_REQ: message = "Room created"; break;
                case MSG_JOIN_ROOM_REQ: message = "Room joined"; break;
                case MSG_INVITE_REQ: message = "Invitation sent"; break;
                case MSG_KICK_REQ: message = "Player kicked"; break;
                case MSG_LEAVE_ROOM_REQ: message = "Left room; returned to lobby"; break;
                case MSG_START_GAME_REQ: message = "Start accepted; waiting for game"; break;
                case MSG_NIGHT_ACTION_REQ: message = "Night action accepted"; break;
                case MSG_CHAT_MSG: message = "Chat sent"; break;
                case MSG_DAY_VOTE_REQ: message = "Vote accepted"; break;
                default: break;
                }
                if (message != NULL) tui_log(TUI_LOG_SUCCESS, "%s", message);
            }
            set_style(ANSI_GREEN);
            switch (client->pending_type) {
            case MSG_CREATE_ROOM_REQ: puts("Room created"); break;
            case MSG_JOIN_ROOM_REQ: puts("Room joined"); break;
            case MSG_INVITE_REQ: puts("Invitation sent"); break;
            case MSG_KICK_REQ: puts("Player kicked"); break;
            case MSG_LEAVE_ROOM_REQ:
                client->room = (struct room_view){0};
                puts("Left room; returned to lobby");
                break;
            case MSG_START_GAME_REQ:
                puts("Start accepted; waiting for role and phase from server");
                break;
            case MSG_NIGHT_ACTION_REQ: puts("Night action accepted"); break;
            case MSG_CHAT_MSG: puts("Chat sent"); break;
            case MSG_DAY_VOTE_REQ: puts("Vote accepted"); break;
            default: return false;
            }
            set_style(ANSI_RESET);
        }
    } else {
        return false;
    }

    client->pending_type = 0;
    return true;
}

static bool read_server(int fd, struct client *client)
{
    struct message_header header;
    size_t target = PROTOCOL_HEADER_SIZE;
    if (client->received >= PROTOCOL_HEADER_SIZE) {
        protocol_decode_header(&header, client->input);
        target += header.length;
    }
    ssize_t count = recv(fd, client->input + client->received,
                         target - client->received, 0);
    if (count <= 0) {
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        }
        fputs("Disconnected from server\n", stderr);
        return false;
    }
    client->received += (size_t)count;
    if (client->received < PROTOCOL_HEADER_SIZE) {
        return true;
    }

    protocol_decode_header(&header, client->input);
    if (header.length > PROTOCOL_MAX_BODY) {
        fputs("Invalid frame from server\n", stderr);
        return false;
    }
    if (client->received < PROTOCOL_HEADER_SIZE + (size_t)header.length) {
        return true;
    }
    if (!handle_message(client, &header)) {
        fputs("Unexpected message from server\n", stderr);
        return false;
    }
    client->received = 0;
    if (tui_is_active()) {
        refresh_tui(client);
    } else if (client->pending_type == 0) {
        prompt(client);
    } else {
        fflush(stdout);
    }
    return true;
}

static bool write_request(int fd, struct client *client)
{
    ssize_t count = send(fd, client->output + client->sent,
                         client->queued - client->sent, MSG_NOSIGNAL);
    if (count <= 0) {
        return count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK);
    }
    client->sent += (size_t)count;
    return true;
}

static int read_command(struct client *client)
{
    char ch;
    ssize_t count = read(STDIN_FILENO, &ch, 1);
    if (count < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            return 1;
        }
        perror("read");
        return -1;
    }
    if (count == 0) {
        return 0;
    }
    if (ch == '\n') {
        if (client->command_too_long) {
            puts("Invalid command line");
            prompt(client);
        } else {
            client->command[client->command_length] = '\0';
            queue_command(client);
        }
        client->command_length = 0;
        client->command_too_long = false;
    } else if (ch == '\0' || client->command_length == sizeof client->command - 1) {
        client->command_too_long = true;
    } else if (!client->command_too_long) {
        client->command[client->command_length++] = ch;
    }
    return 1;
}

static void remove_last_utf8_character(char *text, size_t *length)
{
    if (*length == 0) return;
    --*length;
    while (*length != 0 &&
           ((unsigned char)text[*length] & 0xc0) == 0x80) {
        --*length;
    }
    text[*length] = '\0';
}

static int read_tui_command(struct client *client)
{
    for (;;) {
        wchar_t character = 0;
        enum tui_key key = tui_read_key(&character);
        if (key == TUI_KEY_NONE) break;
        if (key == TUI_KEY_EOF) return 0;
        if (key == TUI_KEY_RESIZE) {
            refresh_tui(client);
            continue;
        }
        if (key == TUI_KEY_BACKSPACE) {
            remove_last_utf8_character(client->command, &client->command_length);
            client->command_too_long = false;
            refresh_tui(client);
            continue;
        }
        if (key == TUI_KEY_ENTER) {
            if (client->command_too_long) {
                show_message(TUI_LOG_ERROR, "Invalid command line");
            } else {
                client->command[client->command_length] = '\0';
                queue_command(client);
            }
            client->command_length = 0;
            client->command[0] = '\0';
            client->command_too_long = false;
            refresh_tui(client);
            continue;
        }
        if (key == TUI_KEY_CHARACTER) {
            char bytes[MB_LEN_MAX];
            mbstate_t state = {0};
            size_t count = wcrtomb(bytes, character, &state);
            if (count == (size_t)-1 ||
                count > sizeof client->command - 1 - client->command_length) {
                client->command_too_long = true;
            } else {
                memcpy(client->command + client->command_length, bytes, count);
                client->command_length += count;
                client->command[client->command_length] = '\0';
            }
            refresh_tui(client);
        }
    }
    return 1;
}

int run_lobby(int fd, const char *username)
{
    setlocale(LC_CTYPE, "");
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl");
        return EXIT_FAILURE;
    }

    struct client client = {.username = username, .last_request_id = 1};
    bool tui_enabled = tui_start();
    rich_terminal = !tui_enabled && isatty(STDOUT_FILENO);
    if (tui_enabled) {
        tui_log(TUI_LOG_SUCCESS, "Logged in as %s", username);
        tui_log(TUI_LOG_SYSTEM, "Press Ctrl+D to exit; type help for commands");
    }
    print_commands();
    prompt(&client);
    int outcome = EXIT_SUCCESS;
    for (;;) {
        struct pollfd sources[] = {
            {.fd = fd, .events = POLLIN},
            {.fd = client.pending_type == 0 ? STDIN_FILENO : -1, .events = POLLIN}
        };
        if (client.sent < client.queued) {
            sources[0].events |= POLLOUT;
        }
        // Chờ đồng thời dữ liệu server và thao tác terminal.
        if (poll(sources, 2, -1) == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            outcome = EXIT_FAILURE;
            break;
        }
        short events = sources[0].revents;
        if ((events & POLLOUT) && !write_request(fd, &client)) {
            fputs("Disconnected from server\n", stderr);
            outcome = EXIT_FAILURE;
            break;
        }
        if (events & POLLIN) {
            if (!read_server(fd, &client)) {
                outcome = EXIT_FAILURE;
                break;
            }
        } else if (events & (POLLERR | POLLHUP | POLLNVAL)) {
            fputs("Disconnected from server\n", stderr);
            outcome = EXIT_FAILURE;
            break;
        }
        if (sources[1].revents & (POLLERR | POLLNVAL)) {
            fputs("Could not read terminal input\n", stderr);
            outcome = EXIT_FAILURE;
            break;
        }
        if (sources[1].revents & (POLLIN | POLLHUP)) {
            int result = tui_enabled ? read_tui_command(&client)
                                     : read_command(&client);
            if (result <= 0) {
                outcome = result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
                break;
            }
        }
    }
    tui_stop();
    return outcome;
}

