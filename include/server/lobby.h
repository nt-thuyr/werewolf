#ifndef WEREWOLF_SERVER_LOBBY_H
#define WEREWOLF_SERVER_LOBBY_H

#include "server/session.h"

struct room {
    uint32_t id;
    char name[PROTOCOL_MAX_NAME + 1];
    uint8_t capacity;
    uint8_t count;
    struct session *members[PROTOCOL_MAX_PLAYERS];
};

struct lobby {
    struct session sessions[MAX_SESSIONS];
    struct room rooms[MAX_SESSIONS];
    uint32_t next_session_id;
    uint32_t next_room_id;
};

void lobby_broadcast_room(const struct room *room);
void lobby_remove_session(struct room *room, struct session *session);
void lobby_list_rooms(const struct lobby *lobby, struct session *session,
                      uint32_t request_id);
uint8_t lobby_handle_request(struct lobby *lobby, struct session *session,
                             const struct message_header *header);

#endif
