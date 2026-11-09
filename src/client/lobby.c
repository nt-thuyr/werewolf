#include "client/lobby.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
    MAX_COMMAND = 128,
    MAX_FRAME = PROTOCOL_HEADER_SIZE + PROTOCOL_MAX_BODY
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

struct client {
    const char *username;
    struct room_view room;
    uint8_t input[MAX_FRAME];
    uint8_t output[PROTOCOL_HEADER_SIZE + PROTOCOL_MAX_NAME + 2];
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

// Đổi lệnh terminal thành yêu cầu có request_id riêng.
static void queue_command(struct client *client)
{
    uint8_t *body = client->output + PROTOCOL_HEADER_SIZE;
    uint8_t type;
    size_t length = 0;
    char *command = strtok(client->command, " \t\r");
    if (command == NULL) {
        prompt();
        return;
    }

    char *argument = strtok(NULL, " \t\r");
    uint32_t number;
    if (strcmp(command, "list") == 0 && argument == NULL) {
        type = MSG_ROOM_LIST_REQ;
    } else if (strcmp(command, "create") == 0 && argument != NULL &&
               strlen(argument) <= PROTOCOL_MAX_NAME &&
               parse_number(strtok(NULL, " \t\r"), &number) &&
               number >= PROTOCOL_MIN_PLAYERS && number <= PROTOCOL_MAX_PLAYERS &&
               strtok(NULL, " \t\r") == NULL) {
        type = MSG_CREATE_ROOM_REQ;
        length = protocol_put_name(body, argument);
        body[length++] = (uint8_t)number;
    } else if (strcmp(command, "join") == 0 && parse_number(argument, &number) &&
               strtok(NULL, " \t\r") == NULL) {
        type = MSG_JOIN_ROOM_REQ;
        protocol_put_u32(body, number);
        length = 4;
    } else {
        puts("Commands: list | create <name> <5-12> | join <room_id>");
        prompt();
        return;
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
        printf("  room_id=%" PRIu32 " %s (%u/%u)\n", id, name, players, capacity);
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
        printf("  %" PRIu32 " %s\n", player->id, player->name);
    }
    return true;
}

static void print_request_error(uint8_t status)
{
    const char *name = "Unknown error";
    switch (status) {
    case ERR_UNAUTHENTICATED: name = "ERR_UNAUTHENTICATED"; break;
    case ERR_UNAUTHORIZED: name = "ERR_UNAUTHORIZED"; break;
    case ERR_ROOM_FULL: name = "ERR_ROOM_FULL"; break;
    case ERR_INVALID_PAYLOAD: name = "ERR_INVALID_PAYLOAD"; break;
    case ERR_ROOM_NOT_FOUND: name = "ERR_ROOM_NOT_FOUND"; break;
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
            puts(client->pending_type == MSG_CREATE_ROOM_REQ ? "Room created" : "Room joined");
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
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl");
        return EXIT_FAILURE;
    }

    struct client client = {.username = username, .last_request_id = 1};
    puts("Commands: list | create <name> <5-12> | join <room_id>");
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

