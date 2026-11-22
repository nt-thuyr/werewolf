#include "client/lobby.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <locale.h>
#include <poll.h>
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

static void prompt(void)
{
    fputs("> ", stdout);
    fflush(stdout);
}

static void print_commands(void)
{
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
        prompt();
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
        prompt();
        return;
    }
    if (strcmp(command, "status") == 0 && argument[0] == '\0') {
        print_game_status(client);
        prompt();
        return;
    }

    if (client->game.active && !client->game.over) {
        if (strcmp(command, "chat") == 0 && argument[0] != '\0') {
            if (!game_self_alive(client)) {
                puts("Dead players can only observe");
                prompt();
                return;
            }
            uint8_t scope;
            if (client->game.phase == PROTOCOL_PHASE_DAY_DISCUSS) {
                scope = PROTOCOL_CHAT_VILLAGE;
            } else if (client->game.phase == PROTOCOL_PHASE_NIGHT &&
                       client->game.role == PROTOCOL_ROLE_WEREWOLF) {
                scope = PROTOCOL_CHAT_WEREWOLF;
            } else {
                puts("Chat is not available in this phase");
                prompt();
                return;
            }
            size_t text_length = strlen(argument);
            if (text_length > PROTOCOL_MAX_CHAT_BYTES ||
                memchr(argument, '\0', text_length) != NULL) {
                puts("Chat message is too long or invalid");
                prompt();
                return;
            }
            body[0] = scope;
            memcpy(body + 1, argument, text_length);
            if (!queue_request(client, MSG_CHAT_MSG, body, 1 + text_length)) {
                puts("Could not queue chat message");
                prompt();
            }
            return;
        }
        if (strcmp(command, "act") == 0) {
            uint32_t target_id;
            if (!game_self_alive(client)) {
                puts("Dead players can only observe");
            } else if (client->game.phase != PROTOCOL_PHASE_NIGHT) {
                puts("Night actions are only available at night");
            } else if (!parse_number(argument, &target_id)) {
                puts("Usage: act <player_id>");
            } else {
                protocol_put_u32(body, target_id);
                if (!queue_request(client, MSG_NIGHT_ACTION_REQ, body, 4)) {
                    puts("Could not queue night action");
                }
            }
            if (client->pending_type == 0) {
                prompt();
            }
            return;
        }
        if (strcmp(command, "vote") == 0) {
            uint32_t target_id;
            if (!game_self_alive(client)) {
                puts("Dead players can only observe");
            } else if (client->game.phase != PROTOCOL_PHASE_DAY_VOTE) {
                puts("Voting is only available during the vote phase");
            } else if (!parse_vote_target(argument, &target_id)) {
                puts("Usage: vote <player_id|0>");
            } else {
                protocol_put_u32(body, target_id);
                if (!queue_request(client, MSG_DAY_VOTE_REQ, body, 4)) {
                    puts("Could not queue vote");
                }
            }
            if (client->pending_type == 0) {
                prompt();
            }
            return;
        }
        puts("Only game actions, chat, vote, status, and help are available during a game");
        prompt();
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
            puts("Join or create a room first");
            prompt();
            return;
        }
        if (client->room.self_id != client->room.host_id) {
            puts("Only the host can start the game");
            prompt();
            return;
        }
        type = MSG_START_GAME_REQ;
    } else {
        print_commands();
        prompt();
        return;
    }

    if (!queue_request(client, type, body, length)) {
        puts("Could not queue request");
        prompt();
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
    printf("Rooms: %u\n", (unsigned)count);
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
        printf("  room_id=%" PRIu32 " ", id);
        print_name(name);
        printf(" (%u/%u)\n", players, capacity);
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
    printf("\nRoom update: room_id=%" PRIu32 " host_id=%" PRIu32 " self_id=%" PRIu32 "\n",
           room.id, room.host_id, room.self_id);
    for (size_t i = 0; i < client->room.count; ++i) {
        const struct player *player = &client->room.players[i];
        printf("  %" PRIu32 " ", player->id);
        print_name(player->name);
        putchar('\n');
    }
    puts(room.self_id == room.host_id ? "You are the host" : "You are a member");
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
        printf("\nGame started. Your role is %s\n", role_name(client->game.role));
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
        fputs("Wolf team:", stdout);
        for (size_t i = 0; i < count; ++i) {
            fputs(" ", stdout);
            print_player_id(client, ids[i]);
        }
        putchar('\n');
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
        printf("\nPhase: %s (%u seconds)\n", phase_name(client->game.phase),
               (unsigned)duration);
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
        fputs("Seer result: ", stdout);
        print_player_id(client, protocol_get_u32(body));
        puts(body[4] ? " is a werewolf" : " is not a werewolf");
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
        fputs("Wolf target: ", stdout);
        print_player_id(client, wolf_id);
        fputs(" chose ", stdout);
        print_player_id(client, target_id);
        putchar('\n');
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
            if (player_id == 0) {
                puts("Night result: nobody died");
            } else {
                fputs("Night result: ", stdout);
                print_player_id(client, player_id);
                puts(" died");
            }
        } else if (player_id == 0) {
            puts("Day result: nobody was executed");
        } else {
            fputs("Day result: ", stdout);
            print_player_id(client, player_id);
            puts(" was executed");
        }
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
        fputs(body[0] == PROTOCOL_CHAT_WEREWOLF ? "[Wolves] " : "[Village] ",
              stdout);
        print_player_id(client, from_id);
        fputs(": ", stdout);
        char message[PROTOCOL_MAX_CHAT_BYTES + 1];
        size_t message_length = header->length - 5;
        memcpy(message, body + 5, message_length);
        message[message_length] = '\0';
        print_name(message);
        putchar('\n');
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
        if (body[0] == PROTOCOL_FACTION_VILLAGE) {
            puts("\nGame over: village wins");
        } else if (body[0] == PROTOCOL_FACTION_WEREWOLF) {
            puts("\nGame over: werewolves win");
        } else {
            puts("\nGame over: game cancelled");
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
    printf("%s (0x%02X)\n", name, (unsigned)status);
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
        fputs("\nInvitation from ", stdout);
        print_name(host);
        printf(": join %" PRIu32 "\n", protocol_get_u32(body));
        return true;
    }
    if (header->type == MSG_KICK_NOTIF) {
        if (header->request_id != 0 || header->status != ERR_SUCCESS ||
            header->length != 1 || body[0] != KICK_REASON_HOST) {
            return false;
        }
        client->room = (struct room_view){0};
        puts("\nKicked by host; returned to lobby");
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
    if (client->pending_type == 0) {
        prompt();
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
            prompt();
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

int run_lobby(int fd, const char *username)
{
    setlocale(LC_CTYPE, "");
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl");
        return EXIT_FAILURE;
    }

    struct client client = {.username = username, .last_request_id = 1};
    print_commands();
    prompt();
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
            return EXIT_FAILURE;
        }
        short events = sources[0].revents;
        if ((events & POLLOUT) && !write_request(fd, &client)) {
            fputs("Disconnected from server\n", stderr);
            return EXIT_FAILURE;
        }
        if (events & POLLIN) {
            if (!read_server(fd, &client)) {
                return EXIT_FAILURE;
            }
        } else if (events & (POLLERR | POLLHUP | POLLNVAL)) {
            fputs("Disconnected from server\n", stderr);
            return EXIT_FAILURE;
        }
        if (sources[1].revents & (POLLERR | POLLNVAL)) {
            fputs("Could not read terminal input\n", stderr);
            return EXIT_FAILURE;
        }
        if (sources[1].revents & (POLLIN | POLLHUP)) {
            int result = read_command(&client);
            if (result <= 0) {
                return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
            }
        }
    }
}

