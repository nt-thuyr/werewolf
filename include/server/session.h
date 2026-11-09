#ifndef WEREWOLF_SESSION_H
#define WEREWOLF_SESSION_H

#include "protocol.h"

enum {
    MAX_SESSIONS = 64,
    MAX_FRAME = PROTOCOL_HEADER_SIZE + PROTOCOL_MAX_BODY
};

struct room;

struct session {
    uint8_t input[MAX_FRAME];
    uint8_t output[2 * MAX_FRAME];
    size_t received;
    size_t sent;
    size_t queued;
    int64_t deadline_ms;
    unsigned invalid_frames;
    bool close_after_reply;
    bool closing;
    uint32_t id;
    char username[PROTOCOL_MAX_USERNAME + 1];
    struct room *room;
};

// Xếp phản hồi vào bộ đệm gửi của phiên.
void session_queue_message(struct session *session, struct message_header header,
                           const uint8_t *body);
int serve_sessions(int listener);

#endif
