#define _POSIX_C_SOURCE 200809L

#include "session.h"
#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum {
    MAX_SESSIONS = 64,
    MAX_INVALID_FRAMES = 5,
    FRAME_TIMEOUT_MS = 10000,
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

static int64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        return -1;
    }
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags == -1 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void queue_message(struct session *session, struct message_header header,
                          const uint8_t *body)
{
    size_t pending = session->queued - session->sent;
    size_t frame_size = PROTOCOL_HEADER_SIZE + (size_t)header.length;
    if (session->closing || session->close_after_reply) {
        return;
    }
    if (frame_size > sizeof session->output - pending) {
        session->closing = true;
        return;
    }
    if (frame_size > sizeof session->output - session->queued) {
        memmove(session->output, session->output + session->sent, pending);
        session->sent = 0;
        session->queued = pending;
    }

    uint8_t *frame = session->output + session->queued;
    protocol_encode_header(frame, &header);
    if (header.length != 0) {
        memcpy(frame + PROTOCOL_HEADER_SIZE, body, header.length);
    }
    session->queued += frame_size;
}

static void broadcast_room(const struct room *room)
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
        queue_message(room->members[i], header, body);
    }
}

static void close_session(struct pollfd *slot, struct session *session)
{
    struct room *room = session->room;
    close(slot->fd);
    slot->fd = -1;
    *session = (struct session){0};
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
        broadcast_room(room);
    }
}

static void close_failed_sessions(struct pollfd *slots, struct lobby *lobby)
{
    bool closed;
    do {
        closed = false;
        for (size_t i = 0; i < MAX_SESSIONS; ++i) {
            if (lobby->sessions[i].closing) {
                close_session(&slots[i + 1], &lobby->sessions[i]);
                closed = true;
            }
        }
    } while (closed);
}

static uint8_t login(struct lobby *lobby, struct session *session,
                     const uint8_t *body, size_t length)
{
    if (length == 0 || length > PROTOCOL_MAX_USERNAME ||
        memchr(body, '\0', length) != NULL) {
        return ERR_INVALID_PAYLOAD;
    }
    if (session->id != 0) {
        return ERR_UNAUTHORIZED;
    }

    char username[PROTOCOL_MAX_USERNAME + 1] = {0};
    memcpy(username, body, length);
    for (size_t i = 0; i < MAX_SESSIONS; ++i) {
        const struct session *other = &lobby->sessions[i];
        if (!other->closing && strcmp(other->username, username) == 0) {
            return ERR_USERNAME_EXIST;
        }
    }
    if (lobby->next_session_id == 0) {
        return ERR_INTERNAL;
    }
    session->id = lobby->next_session_id++;
    memcpy(session->username, username, sizeof username);
    return ERR_SUCCESS;
}

static void list_rooms(const struct lobby *lobby, struct session *session,
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
    queue_message(session, header, body);
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

static uint8_t handle_request(struct lobby *lobby, struct session *session,
                              const struct message_header *header)
{
    const uint8_t *body = session->input + PROTOCOL_HEADER_SIZE;
    if (header->status != 0 || header->request_id == 0) {
        return ERR_INVALID_PAYLOAD;
    }
    if (header->type == MSG_LOGIN_REQ) {
        return login(lobby, session, body, header->length);
    }

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

static bool read_request(int fd, struct lobby *lobby, struct session *session,
                         int64_t now)
{
    struct message_header header;
    size_t target = PROTOCOL_HEADER_SIZE;
    if (session->received >= PROTOCOL_HEADER_SIZE) {
        protocol_decode_header(&header, session->input);
        target += header.length;
    }

    ssize_t count = recv(fd, session->input + session->received,
                         target - session->received, 0);
    if (count <= 0) {
        return count < 0 && (errno == EINTR || errno == EAGAIN ||
                             errno == EWOULDBLOCK);
    }
    if (session->received == 0) {
        session->deadline_ms = now + FRAME_TIMEOUT_MS;
    }
    session->received += (size_t)count;
    if (session->received < PROTOCOL_HEADER_SIZE) {
        return true;
    }

    protocol_decode_header(&header, session->input);
    if (header.length > PROTOCOL_MAX_BODY) {
        return false;
    }
    if (session->received < (size_t)PROTOCOL_HEADER_SIZE + header.length) {
        return true;
    }

    uint8_t status = handle_request(lobby, session, &header);
    if (status == ERR_INVALID_PAYLOAD) {
        ++session->invalid_frames;
    } else {
        session->invalid_frames = 0;
    }

    if (status == ERR_SUCCESS && header.type == MSG_ROOM_LIST_REQ) {
        list_rooms(lobby, session, header.request_id);
    } else {
        struct message_header reply = {
            .type = MSG_RESPONSE,
            .request_id = header.request_id,
            .status = status
        };
        queue_message(session, reply, NULL);
        if (status == ERR_SUCCESS &&
            (header.type == MSG_CREATE_ROOM_REQ || header.type == MSG_JOIN_ROOM_REQ)) {
            broadcast_room(session->room);
        }
    }
    session->received = 0;
    session->deadline_ms = 0;
    session->close_after_reply = session->invalid_frames >= MAX_INVALID_FRAMES;
    return true;
}

static bool write_reply(int fd, struct session *session)
{
    ssize_t count = send(fd, session->output + session->sent,
                         session->queued - session->sent, MSG_NOSIGNAL);
    if (count <= 0) {
        return count < 0 && (errno == EINTR || errno == EAGAIN ||
                             errno == EWOULDBLOCK);
    }
    session->sent += (size_t)count;
    if (session->sent == session->queued) {
        session->sent = 0;
        session->queued = 0;
        return !session->close_after_reply;
    }
    return true;
}

static void accept_session(struct pollfd *slots)
{
    int client = accept(slots[0].fd, NULL, NULL);
    if (client == -1) {
        return;
    }
    if (set_nonblocking(client) == 0) {
        for (size_t i = 1; i <= MAX_SESSIONS; ++i) {
            if (slots[i].fd == -1) {
                slots[i].fd = client;
                return;
            }
        }
    }
    close(client);
}

int serve_sessions(int listener)
{
    struct pollfd slots[MAX_SESSIONS + 1] = {{.fd = listener, .events = POLLIN}};
    struct lobby lobby = {.next_session_id = 1, .next_room_id = 1};
    for (size_t i = 1; i <= MAX_SESSIONS; ++i) {
        slots[i].fd = -1;
    }
    if (set_nonblocking(listener) == -1) {
        perror("fcntl");
        return -1;
    }

    for (;;) {
        int64_t now = monotonic_ms();
        if (now < 0) {
            perror("clock_gettime");
            break;
        }
        int timeout = -1;
        for (size_t i = 1; i <= MAX_SESSIONS; ++i) {
            struct session *session = &lobby.sessions[i - 1];
            slots[i].events = session->queued != 0 ? POLLOUT : POLLIN;
            if (session->deadline_ms != 0) {
                int64_t remaining = session->deadline_ms - now;
                if (remaining <= 0) {
                    timeout = 0;
                } else if (timeout == -1 || remaining < timeout) {
                    timeout = (int)remaining;
                }
            }
        }

        if (poll(slots, MAX_SESSIONS + 1, timeout) == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }
        now = monotonic_ms();
        if (now < 0) {
            perror("clock_gettime");
            break;
        }
        for (size_t i = 1; i <= MAX_SESSIONS; ++i) {
            struct pollfd *slot = &slots[i];
            struct session *session = &lobby.sessions[i - 1];
            if (slot->fd == -1 || session->closing) {
                continue;
            }
            short events = slot->revents;
            if ((session->deadline_ms != 0 && now >= session->deadline_ms) ||
                (events & (POLLERR | POLLNVAL))) {
                session->closing = true;
            } else if (session->queued != 0 && (events & POLLOUT)) {
                session->closing = !write_reply(slot->fd, session);
            } else if (session->queued == 0 && (events & POLLIN)) {
                if (!read_request(slot->fd, &lobby, session, now)) {
                    session->closing = true;
                }
            } else if (events & POLLHUP) {
                session->closing = true;
            }
        }
        close_failed_sessions(slots, &lobby);
        if (slots[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fputs("Listening socket failed\n", stderr);
            break;
        }
        if (slots[0].revents & POLLIN) {
            accept_session(slots);
        }
    }

    for (size_t i = 1; i <= MAX_SESSIONS; ++i) {
        if (slots[i].fd != -1) {
            close(slots[i].fd);
        }
    }
    return -1;
}
