#ifndef WEREWOLF_SERVER_LOBBY_H
#define WEREWOLF_SERVER_LOBBY_H

#include "server/game.h"
#include "server/session.h"

struct room {
    uint32_t id;
    char name[PROTOCOL_MAX_NAME + 1];
    uint8_t capacity;
    uint8_t count;
    struct session *members[PROTOCOL_MAX_PLAYERS];
    struct game game;
};

struct lobby {
    struct session sessions[MAX_SESSIONS];
    struct room rooms[MAX_SESSIONS];
    uint32_t next_session_id;
    uint32_t next_room_id;
};

void lobby_remove_session(struct session *session, uint64_t now_ms);
void lobby_list_rooms(const struct lobby *lobby, struct session *session,
                      uint32_t request_id);
uint8_t lobby_handle_request(struct lobby *lobby, struct session *session,
                             const struct message_header *header,
                             uint64_t now_ms, struct session **target,
                             struct game_event_batch *events);
// Gọi sau khi đã xếp phản hồi thành công để giữ thứ tự thông báo trên TCP.
void lobby_finish_request(struct session *session, uint8_t type, uint64_t now_ms,
                          struct session *target);
void lobby_send_game_events(struct room *room,
                            const struct game_event_batch *events);
uint64_t lobby_next_game_deadline(const struct lobby *lobby);
void lobby_handle_game_timeouts(struct lobby *lobby, uint64_t now_ms);

#endif
