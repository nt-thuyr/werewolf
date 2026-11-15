#define _GNU_SOURCE

#include "net_config.h"
#include "server/session.h"
#include "server/lobby.h"
#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum {
    MAX_INVALID_FRAMES = 5,
    FRAME_TIMEOUT_MS = 10000,
    ACCEPT_RETRY_MS = 250
};

static volatile sig_atomic_t stop_requested;

static void request_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static int prepare_signals(sigset_t *wait_mask)
{
    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &stop_signals, wait_mask) == -1) {
        return -1;
    }
    sigdelset(wait_mask, SIGINT);
    sigdelset(wait_mask, SIGTERM);

    struct sigaction action = {.sa_handler = request_stop};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) == -1 ||
        sigaction(SIGTERM, &action, NULL) == -1) {
        return -1;
    }
    action.sa_handler = SIG_IGN;
    return sigaction(SIGPIPE, &action, NULL);
}

static void log_frame_error(int log_fd, const char *message)
{
    size_t length = strlen(message);
    if (log_fd != -1) {
        (void)write(log_fd, message, length);
    } else {
        (void)send(STDERR_FILENO, message, length, MSG_DONTWAIT | MSG_NOSIGNAL);
    }
}

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

void session_queue_message(struct session *session, struct message_header header,
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

static void close_session(struct pollfd *slot, struct session *session)
{
    struct room *room = session->room;
    close(slot->fd);
    slot->fd = -1;
    *session = (struct session){0};
    // Gỡ thành viên sau khi đóng và xóa trạng thái phiên.
    lobby_remove_session(room, session);
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

    return lobby_handle_request(lobby, session, header);
}

static bool read_request(int fd, struct lobby *lobby, struct session *session,
                         int64_t now, int log_fd)
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
        char message[96];
        snprintf(message, sizeof message,
                 "Closing fd %d: frame body length %u exceeds %d bytes\n",
                 fd, (unsigned)header.length, PROTOCOL_MAX_BODY);
        log_frame_error(log_fd, message);
        return false;
    }
    if (session->received < (size_t)PROTOCOL_HEADER_SIZE + header.length) {
        return true;
    }

    // Chỉ xử lý yêu cầu khi đã nhận đủ một khung.
    uint8_t status = handle_request(lobby, session, &header);
    if (status == ERR_INVALID_PAYLOAD) {
        ++session->invalid_frames;
    } else {
        session->invalid_frames = 0;
    }

    // Xếp phản hồi trước, rồi mới đẩy cập nhật phòng.
    if (status == ERR_SUCCESS && header.type == MSG_ROOM_LIST_REQ) {
        lobby_list_rooms(lobby, session, header.request_id);
    } else {
        struct message_header reply = {
            .type = MSG_RESPONSE,
            .request_id = header.request_id,
            .status = status
        };
        session_queue_message(session, reply, NULL);
        if (status == ERR_SUCCESS &&
            (header.type == MSG_CREATE_ROOM_REQ || header.type == MSG_JOIN_ROOM_REQ)) {
            lobby_broadcast_room(session->room);
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

static bool accept_session(int listener, struct pollfd *slots)
{
    int client = accept(listener, NULL, NULL);
    if (client == -1) {
        return errno != EMFILE && errno != ENFILE &&
               errno != ENOBUFS && errno != ENOMEM;
    }
    if (set_nonblocking(client) == 0) {
        for (size_t i = 1; i <= MAX_SESSIONS; ++i) {
            if (slots[i].fd == -1) {
                slots[i].fd = client;
                return true;
            }
        }
    }
    close(client);
    return true;
}

int serve_sessions(int listener, const char *bind_ip)
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
    sigset_t wait_mask;
    if (prepare_signals(&wait_mask) == -1) {
        perror("signal setup");
        return -1;
    }
    // Mở mô tả riêng để không đổi cờ I/O của stderr được kế thừa.
    int log_fd = open("/proc/self/fd/2", O_WRONLY | O_NONBLOCK | O_APPEND | O_CLOEXEC);
    printf("Listening on %s:%d\n", bind_ip, WEREWOLF_PORT);
    fflush(stdout);

    int64_t accept_retry_at = 0;
    while (!stop_requested) {
        int64_t now = monotonic_ms();
        if (now < 0) {
            perror("clock_gettime");
            break;
        }
        int timeout = -1;
        slots[0].fd = listener;
        if (now < accept_retry_at) {
            slots[0].fd = -1;
            timeout = (int)(accept_retry_at - now);
        }
        nfds_t count = 1;
        for (size_t i = 1; i <= MAX_SESSIONS; ++i) {
            if (slots[i].fd == -1) {
                continue;
            }
            count = i + 1;
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

        struct timespec wait_time = {
            .tv_sec = timeout < 0 ? 0 : timeout / 1000,
            .tv_nsec = timeout < 0 ? 0 : (timeout % 1000) * 1000000L
        };
        // Bỏ chặn tín hiệu cùng lúc bắt đầu chờ, tránh bỏ lỡ tín hiệu dừng.
        int ready = ppoll(slots, count, timeout < 0 ? NULL : &wait_time, &wait_mask);
        if (stop_requested) {
            break;
        }
        if (ready == -1) {
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
        for (size_t i = 1; i < count; ++i) {
            struct pollfd *slot = &slots[i];
            struct session *session = &lobby.sessions[i - 1];
            if (slot->fd == -1 || session->closing) {
                continue;
            }
            short events = slot->revents;
            if (session->deadline_ms != 0 && now >= session->deadline_ms) {
                char message[80];
                snprintf(message, sizeof message,
                         "Closing fd %d: incomplete frame timed out\n", slot->fd);
                log_frame_error(log_fd, message);
                session->closing = true;
            } else if (events & (POLLERR | POLLNVAL)) {
                session->closing = true;
            } else if (session->queued != 0 && (events & POLLOUT)) {
                session->closing = !write_reply(slot->fd, session);
            } else if (session->queued == 0 && (events & POLLIN)) {
                if (!read_request(slot->fd, &lobby, session, now, log_fd)) {
                    session->closing = true;
                }
            } else if (events & POLLHUP) {
                session->closing = true;
            }
        }
        // Dọn phiên lỗi và cập nhật phòng trước khi nhận kết nối mới.
        close_failed_sessions(slots, &lobby);
        if (slots[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fputs("Listening socket failed\n", stderr);
            break;
        }
        if ((slots[0].revents & POLLIN) && !accept_session(listener, slots)) {
            accept_retry_at = now + ACCEPT_RETRY_MS;
        }
    }

    for (size_t i = 1; i <= MAX_SESSIONS; ++i) {
        if (slots[i].fd != -1) {
            close(slots[i].fd);
        }
    }
    if (log_fd != -1) {
        close(log_fd);
    }
    return stop_requested ? 0 : -1;
}
