#include "server/lobby.h"

#include <string.h>

// Thành viên đầu tiên luôn là Chủ phòng.
void lobby_broadcast_room(const struct room *room)
{
    uint8_t body[9 + PROTOCOL_MAX_PLAYERS * (5 + PROTOCOL_MAX_NAME)];
    protocol_put_u32(body, room->id);
    protocol_put_u32(body + 4, room->members[0]->id);
    body[8] = room->count;
    size_t length = 9;
    for (size_t i = 0; i < room->count; ++i) {
        const struct session *member = room->members[i];
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
void lobby_remove_session(struct room *room, struct session *session)
{
    if (room == NULL) {
        return;
    }

    for (size_t i = 0; i < room->count; ++i) {
        if (room->members[i] == session) {
            --room->count;
            memmove(room->members + i, room->members + i + 1,
                    (room->count - i) * sizeof room->members[0]);
            room->members[room->count] = NULL;
            break;
        }
    }
    if (room->count == 0) {
        *room = (struct room){0};
    } else {
        lobby_broadcast_room(room);
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
        if (room->count == room->capacity) {
            return ERR_ROOM_FULL;
        }
        room->members[room->count++] = session;
        session->room = room;
        return ERR_SUCCESS;
    }
    return ERR_ROOM_NOT_FOUND;
}

uint8_t lobby_handle_request(struct lobby *lobby, struct session *session,
                              const struct message_header *header)
{
    const uint8_t *body = session->input + PROTOCOL_HEADER_SIZE;
    char name[PROTOCOL_MAX_NAME + 1];
    size_t offset = 0;
    uint8_t capacity = 0;
    switch (header->type) {
    case MSG_ROOM_LIST_REQ:
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
        if (header->length != 4) {
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
    if (session->room != NULL) {
        return ERR_UNAUTHORIZED;
    }
    if (header->type == MSG_CREATE_ROOM_REQ) {
        return create_room(lobby, session, name, capacity);
    }
    if (header->type == MSG_JOIN_ROOM_REQ) {
        return join_room(lobby, session, protocol_get_u32(body));
    }
    return ERR_SUCCESS;
}

