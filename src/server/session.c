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
    FRAME_TIMEOUT_MS = 10000
};

struct session {
    uint8_t input[PROTOCOL_HEADER_SIZE + PROTOCOL_MAX_BODY];
    uint8_t reply[PROTOCOL_HEADER_SIZE];
    size_t received;
    size_t sent;
    int64_t deadline_ms;
    unsigned invalid_frames;
    bool reply_pending;
    bool close_after_reply;
    char username[PROTOCOL_MAX_USERNAME + 1];
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

static void close_session(struct pollfd *slot, struct session *session)
{
    close(slot->fd);
    slot->fd = -1;
    *session = (struct session){0};
}

static uint8_t handle_request(struct session *session,
                              const struct session *sessions,
                              const struct message_header *header)
{
    const uint8_t *name = session->input + PROTOCOL_HEADER_SIZE;
    if (header->type != MSG_LOGIN_REQ || header->status != 0 ||
        header->length == 0 || header->length > PROTOCOL_MAX_USERNAME ||
        memchr(name, '\0', header->length) != NULL) {
        return ERR_INVALID_PAYLOAD;
    }
    if (session->username[0] != '\0') {
        return ERR_UNAUTHORIZED;
    }

    char username[PROTOCOL_MAX_USERNAME + 1] = {0};
    memcpy(username, name, header->length);
    for (size_t i = 0; i < MAX_SESSIONS; ++i) {
        if (strcmp(sessions[i].username, username) == 0) {
            return ERR_USERNAME_EXIST;
        }
    }
    memcpy(session->username, username, sizeof username);
    return ERR_SUCCESS;
}

static bool read_request(int fd, struct session *session,
                         const struct session *sessions, int64_t now)
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

    uint8_t status = handle_request(session, sessions, &header);
    if (status == ERR_INVALID_PAYLOAD) {
        ++session->invalid_frames;
    } else {
        session->invalid_frames = 0;
    }
    struct message_header reply = {
        .type = MSG_RESPONSE,
        .request_id = header.request_id,
        .status = status
    };
    protocol_encode_header(session->reply, &reply);
    session->received = 0;
    session->sent = 0;
    session->deadline_ms = 0;
    session->reply_pending = true;
    session->close_after_reply = session->invalid_frames >= MAX_INVALID_FRAMES;
    return true;
}

static bool write_reply(int fd, struct session *session)
{
    ssize_t count = send(fd, session->reply + session->sent,
                         sizeof session->reply - session->sent, MSG_NOSIGNAL);
    if (count <= 0) {
        return count < 0 && (errno == EINTR || errno == EAGAIN ||
                             errno == EWOULDBLOCK);
    }
    session->sent += (size_t)count;
    session->reply_pending = session->sent < sizeof session->reply;
    return session->reply_pending || !session->close_after_reply;
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
    struct session sessions[MAX_SESSIONS] = {0};
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
            struct session *session = &sessions[i - 1];
            slots[i].events = session->reply_pending ? POLLOUT : POLLIN;
            if (session->deadline_ms != 0) {
                int64_t remaining = session->deadline_ms - now;
                if (remaining <= 0) {
                    close_session(&slots[i], session);
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
            struct session *session = &sessions[i - 1];
            if (slot->fd == -1) {
                continue;
            }
            short events = slot->revents;
            bool keep = true;
            if ((session->deadline_ms != 0 && now >= session->deadline_ms) ||
                (events & (POLLERR | POLLNVAL))) {
                keep = false;
            } else if (session->reply_pending && (events & POLLOUT)) {
                keep = write_reply(slot->fd, session);
            } else if (!session->reply_pending && (events & POLLIN)) {
                keep = read_request(slot->fd, session, sessions, now);
            } else if (events & POLLHUP) {
                keep = false;
            }
            if (!keep) {
                close_session(slot, session);
            }
        }
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
            close_session(&slots[i], &sessions[i - 1]);
        }
    }
    return -1;
}
