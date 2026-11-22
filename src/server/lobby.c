#include "server/lobby.h"

#include <string.h>
#include <sys/random.h>

// Thành viên đầu tiên luôn là Chủ phòng.
static void broadcast_room(const struct room *room)
{
    uint8_t body[9 + PROTOCOL_MAX_PLAYERS * (5 + PROTOCOL_MAX_NAME)];
    protocol_put_u32(body, room->id);
    protocol_put_u32(body + 4, room->members[0]->id);
    body[8] = room->count;
    size_t length = 9;
    for (size_t i = 0; i < room->count; ++i) {
        const struct session *member = room->members[i];
        // Lần gỡ phiên cuối cùng đang đóng sẽ gửi danh sách đã cập nhật.
        if (member->closing) {
            return;
        }
        protocol_put_u32(body + length, member->id);
        length += 4;
        length += protocol_put_name(body + length, member->username);
    }
    struct message_header header = {
        .type = MSG_ROOM_UPDATE,
        .length = (uint16_t)length
    };
    for (size_t i = 0; i < room->count; ++i) {
        session_queue_message(room->members[i], header, body);
    }
}

// Dọn phòng rỗng hoặc thông báo danh sách thành viên còn lại.
void lobby_remove_session(struct session *session, uint64_t now_ms)
{
    struct room *room = session->room;
    if (room == NULL) {
        return;
    }

    for (size_t i = 0; i < room->count; ++i) {
        if (room->members[i] == session) {
            --room->count;
            memmove(room->members + i, room->members + i + 1,
                    (room->count - i) * sizeof room->members[0]);
            room->members[room->count] = NULL;
            session->room = NULL;
            break;
        }
    }
    if (room->count != 0) {
        broadcast_room(room);
    }

    if (room->game.initialized && game_find_player(&room->game, session->id) != NULL) {
        struct game_event_batch events = {0};
        if (game_handle_player_left(&room->game, session->id, now_ms, &events) ==
            GAME_RESULT_OK && room->count != 0) {
            lobby_send_game_events(room, &events);
        }
    }
    if (room->count == 0) {
        *room = (struct room){0};
    }
}

void lobby_list_rooms(const struct lobby *lobby, struct session *session,
                       uint32_t request_id)
{
    uint8_t body[2 + MAX_SESSIONS * (7 + PROTOCOL_MAX_NAME)];
    size_t length = 2;
    uint16_t count = 0;
    for (size_t i = 0; i < MAX_SESSIONS; ++i) {
        const struct room *room = &lobby->rooms[i];
        if (room->id == 0) {
            continue;
        }
        ++count;
        protocol_put_u32(body + length, room->id);
        length += 4;
        length += protocol_put_name(body + length, room->name);
        body[length++] = room->count;
        body[length++] = room->capacity;
    }
    protocol_put_u16(body, count);
    struct message_header header = {
        .type = MSG_ROOM_LIST_RESP,
        .length = (uint16_t)length,
        .request_id = request_id
    };
    session_queue_message(session, header, body);
}

static uint8_t create_room(struct lobby *lobby, struct session *session,
                           const char *name, uint8_t capacity)
{
    if (lobby->next_room_id == 0) {
        return ERR_INTERNAL;
    }
    for (size_t i = 0; i < MAX_SESSIONS; ++i) {
        struct room *room = &lobby->rooms[i];
        if (room->id == 0) {
            room->id = lobby->next_room_id++;
            strcpy(room->name, name);
            room->capacity = capacity;
            room->count = 1;
            room->members[0] = session;
            session->room = room;
            return ERR_SUCCESS;
        }
    }
    return ERR_INTERNAL;
}

static uint8_t join_room(struct lobby *lobby, struct session *session,
                         uint32_t room_id)
{
    for (size_t i = 0; room_id != 0 && i < MAX_SESSIONS; ++i) {
        struct room *room = &lobby->rooms[i];
        if (room->id != room_id) {
            continue;
        }
        if (room->game.initialized) {
            return ERR_GAME_RUNNING;
        }
        if (room->count == room->capacity) {
            return ERR_ROOM_FULL;
        }
        room->members[room->count++] = session;
        session->room = room;
        return ERR_SUCCESS;
    }
    return ERR_ROOM_NOT_FOUND;
}

static struct session *find_invitee(struct lobby *lobby, const char *name)
{
    for (size_t i = 0; i < MAX_SESSIONS; ++i) {
        struct session *candidate = &lobby->sessions[i];
        if (candidate->id != 0 && candidate->room == NULL &&
            !candidate->closing && !candidate->close_after_reply &&
            strcmp(candidate->username, name) == 0) {
            return candidate;
        }
    }
    return NULL;
}

static struct session *find_kick_target(const struct room *room, uint32_t id)
{
    // Bỏ qua phần tử đầu tiên vì đó là Chủ phòng.
    for (size_t i = 1; i < room->count; ++i) {
        struct session *candidate = room->members[i];
        if (candidate->id == id && !candidate->closing &&
            !candidate->close_after_reply) {
            return candidate;
        }
    }
    return NULL;
}

static uint8_t start_game(struct room *room, uint64_t now_ms,
                          struct game_event_batch *events)
{
    uint32_t player_ids[PROTOCOL_MAX_PLAYERS];
    size_t count = 0;
    for (size_t i = 0; i < room->count; ++i) {
        const struct session *member = room->members[i];
        if (!member->closing && !member->close_after_reply) {
            player_ids[count++] = member->id;
        }
    }
    if (count < PROTOCOL_MIN_PLAYERS) {
        return ERR_NOT_ENOUGH_PLAYERS;
    }

    uint64_t seed;
    if (getrandom(&seed, sizeof seed, GRND_NONBLOCK) != sizeof seed) {
        return ERR_INTERNAL;
    }
    struct game_start_input input = {
        .player_ids = player_ids,
        .player_count = count,
        .now_ms = now_ms,
        .random_seed = seed == 0 ? 1 : seed
    };
    return game_init(&room->game, &input, events);
}

uint8_t lobby_handle_request(struct lobby *lobby, struct session *session,
                             const struct message_header *header,
                             uint64_t now_ms, struct session **target,
                             struct game_event_batch *events)
{
    const uint8_t *body = session->input + PROTOCOL_HEADER_SIZE;
    char name[PROTOCOL_MAX_NAME + 1];
    size_t offset = 0;
    uint8_t capacity = 0;
    struct game_command command = {0};
    switch (header->type) {
    case MSG_ROOM_LIST_REQ:
    case MSG_LEAVE_ROOM_REQ:
    case MSG_START_GAME_REQ:
        if (header->length != 0) {
            return ERR_INVALID_PAYLOAD;
        }
        break;
    case MSG_CREATE_ROOM_REQ:
        if (!protocol_get_name(body, header->length, &offset, name) ||
            offset + 1 != header->length) {
            return ERR_INVALID_PAYLOAD;
        }
        capacity = body[offset];
        if (capacity < PROTOCOL_MIN_PLAYERS || capacity > PROTOCOL_MAX_PLAYERS) {
            return ERR_INVALID_PAYLOAD;
        }
        break;
    case MSG_JOIN_ROOM_REQ:
    case MSG_KICK_REQ:
        if (header->length != 4) {
            return ERR_INVALID_PAYLOAD;
        }
        break;
    case MSG_NIGHT_ACTION_REQ:
    case MSG_DAY_VOTE_REQ:
        if (header->length != 4) {
            return ERR_INVALID_PAYLOAD;
        }
        if (header->type == MSG_NIGHT_ACTION_REQ) {
            command.type = GAME_COMMAND_NIGHT_ACTION;
            command.data.night_action.target_player_id = protocol_get_u32(body);
        } else {
            command.type = GAME_COMMAND_DAY_VOTE;
            command.data.day_vote.target_player_id = protocol_get_u32(body);
        }
        break;
    case MSG_CHAT_MSG:
        if (header->length < 2 || header->length > 1 + GAME_MAX_CHAT_BYTES ||
            body[0] > GAME_CHAT_WEREWOLF ||
            memchr(body + 1, '\0', header->length - 1) != NULL) {
            return ERR_INVALID_PAYLOAD;
        }
        command.type = GAME_COMMAND_CHAT;
        command.data.chat.scope = body[0];
        command.data.chat.message = body + 1;
        command.data.chat.message_length = header->length - 1;
        break;
    case MSG_INVITE_REQ:
        if (!protocol_get_name(body, header->length, &offset, name) ||
            offset != header->length) {
            return ERR_INVALID_PAYLOAD;
        }
        break;
    default:
        return ERR_INVALID_PAYLOAD;
    }

    // Kiểm tra phiên và quyền trước khi thao tác phòng.
    if (session->id == 0) {
        return ERR_UNAUTHENTICATED;
    }
    if (header->type == MSG_LEAVE_ROOM_REQ) {
        return session->room == NULL ? ERR_UNAUTHORIZED : ERR_SUCCESS;
    }
    struct room *room = session->room;
    if (command.type != GAME_COMMAND_NONE) {
        if (room == NULL) {
            return ERR_UNAUTHORIZED;
        }
        if (!room->game.initialized) {
            return ERR_WRONG_PHASE;
        }
        return game_handle_command(&room->game, session->id, now_ms, &command, events);
    }
    if (header->type == MSG_INVITE_REQ || header->type == MSG_KICK_REQ ||
        header->type == MSG_START_GAME_REQ) {
        if (room == NULL || room->members[0] != session) {
            return ERR_UNAUTHORIZED;
        }
        if (room->game.initialized) {
            return ERR_GAME_RUNNING;
        }
        if (header->type == MSG_START_GAME_REQ) {
            return start_game(room, now_ms, events);
        }
        *target = header->type == MSG_INVITE_REQ
            ? find_invitee(lobby, name)
            : find_kick_target(room, protocol_get_u32(body));
        return *target == NULL ? ERR_INVALID_TARGET : ERR_SUCCESS;
    }
    if (room != NULL) {
        return ERR_UNAUTHORIZED;
    }
    switch (header->type) {
    case MSG_ROOM_LIST_REQ:
        return ERR_SUCCESS;
    case MSG_CREATE_ROOM_REQ:
        return create_room(lobby, session, name, capacity);
    case MSG_JOIN_ROOM_REQ:
        return join_room(lobby, session, protocol_get_u32(body));
    default:
        return ERR_INVALID_PAYLOAD;
    }
}

static void send_game_event(const struct room *room, const struct game_event *event)
{
    struct message_header header = {0};
    uint8_t body[PROTOCOL_MAX_BODY];
    switch (event->type) {
    case GAME_EVENT_ROLE_ASSIGN:
        header.type = MSG_ROLE_ASSIGN;
        header.length = 1;
        body[0] = event->data.role_assign.role;
        break;
    case GAME_EVENT_WOLF_TEAM_INFO:
        header.type = MSG_WOLF_TEAM_INFO;
        body[0] = event->data.wolf_team_info.wolf_count;
        header.length = 1 + 4 * body[0];
        for (size_t i = 0; i < body[0]; ++i) {
            protocol_put_u32(body + 1 + 4 * i, event->data.wolf_team_info.wolf_ids[i]);
        }
        break;
    case GAME_EVENT_PHASE_CHANGE:
        header.type = MSG_PHASE_CHANGE;
        header.length = 11;
        body[0] = event->data.phase_change.phase;
        protocol_put_u16(body + 1, event->data.phase_change.duration_sec);
        protocol_put_u64(body + 3, event->data.phase_change.deadline_ms);
        break;
    case GAME_EVENT_SEER_RESULT:
        header.type = MSG_SEER_RESULT;
        header.length = 5;
        protocol_put_u32(body, event->data.seer_result.target_player_id);
        body[4] = event->data.seer_result.is_werewolf;
        break;
    case GAME_EVENT_WOLF_TARGET_UPDATE:
        header.type = MSG_WOLF_TARGET_UPDATE;
        header.length = 8;
        protocol_put_u32(body, event->data.wolf_target_update.wolf_id);
        protocol_put_u32(body + 4, event->data.wolf_target_update.target_player_id);
        break;
    case GAME_EVENT_NIGHT_RESULT:
        header.type = MSG_NIGHT_RESULT;
        header.length = 4;
        protocol_put_u32(body, event->data.night_result.killed_player_id);
        break;
    case GAME_EVENT_DAY_RESULT:
        header.type = MSG_DAY_RESULT;
        header.length = 4;
        protocol_put_u32(body, event->data.day_result.executed_player_id);
        break;
    case GAME_EVENT_CHAT:
        header.type = MSG_CHAT_NOTIF;
        header.length = 5 + event->data.chat.message_length;
        body[0] = event->data.chat.scope;
        protocol_put_u32(body + 1, event->data.chat.from_player_id);
        memcpy(body + 5, event->data.chat.message, event->data.chat.message_length);
        break;
    case GAME_EVENT_GAME_OVER:
        header.type = MSG_GAME_OVER;
        header.length = 1 + event->data.game_over.summary_length;
        body[0] = event->data.game_over.winner;
        memcpy(body + 1, event->data.game_over.summary,
               event->data.game_over.summary_length);
        break;
    default:
        return;
    }

    for (size_t i = 0; i < event->recipient_count; ++i) {
        for (size_t j = 0; j < room->count; ++j) {
            if (room->members[j]->id == event->recipient_ids[i]) {
                session_queue_message(room->members[j], header, body);
                break;
            }
        }
    }
}

void lobby_send_game_events(struct room *room,
                            const struct game_event_batch *events)
{
    for (size_t i = 0; i < events->count; ++i) {
        send_game_event(room, &events->events[i]);
    }
    if (game_is_over(&room->game)) {
        room->game = (struct game){0};
        broadcast_room(room);
    }
}

uint64_t lobby_next_game_deadline(const struct lobby *lobby)
{
    uint64_t next = UINT64_MAX;
    for (size_t i = 0; i < MAX_SESSIONS; ++i) {
        uint64_t deadline = game_next_deadline_ms(&lobby->rooms[i].game);
        if (deadline < next) {
            next = deadline;
        }
    }
    return next;
}

void lobby_handle_game_timeouts(struct lobby *lobby, uint64_t now_ms)
{
    for (size_t i = 0; i < MAX_SESSIONS; ++i) {
        struct room *room = &lobby->rooms[i];
        if (game_next_deadline_ms(&room->game) <= now_ms) {
            struct game_event_batch events = {0};
            if (game_handle_timeout(&room->game, now_ms, &events) == GAME_RESULT_OK) {
                lobby_send_game_events(room, &events);
            }
        }
    }
}

void lobby_finish_request(struct session *session, uint8_t type, uint64_t now_ms,
                          struct session *target)
{
    switch (type) {
    case MSG_CREATE_ROOM_REQ:
    case MSG_JOIN_ROOM_REQ:
        broadcast_room(session->room);
        break;
    case MSG_LEAVE_ROOM_REQ:
        lobby_remove_session(session, now_ms);
        break;
    case MSG_INVITE_REQ: {
        uint8_t body[4 + 1 + PROTOCOL_MAX_NAME];
        protocol_put_u32(body, session->room->id);
        size_t length = 4 + protocol_put_name(body + 4, session->username);
        struct message_header notice = {
            .type = MSG_INVITE_NOTIF,
            .length = (uint16_t)length
        };
        session_queue_message(target, notice, body);
        break;
    }
    case MSG_KICK_REQ: {
        const uint8_t reason = KICK_REASON_HOST;
        struct message_header notice = {
            .type = MSG_KICK_NOTIF,
            .length = sizeof reason
        };
        session_queue_message(target, notice, &reason);
        lobby_remove_session(target, now_ms);
        break;
    }
    }
}

