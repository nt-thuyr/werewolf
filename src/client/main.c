#include "net_config.h"
#include "client/lobby.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int login(int fd, const char *username, size_t length)
{
    uint8_t request[PROTOCOL_HEADER_SIZE + PROTOCOL_MAX_USERNAME];
    struct message_header header = {
        .type = MSG_LOGIN_REQ,
        .length = (uint16_t)length,
        .request_id = 1
    };
    protocol_encode_header(request, &header);
    memcpy(request + PROTOCOL_HEADER_SIZE, username, length);

    size_t request_size = PROTOCOL_HEADER_SIZE + length;
    for (size_t sent = 0; sent < request_size;) {
        ssize_t count = send(fd, request + sent, request_size - sent,
                             MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            fputs("Could not send login request\n", stderr);
            return EXIT_FAILURE;
        }
        sent += (size_t)count;
    }

    uint8_t reply[PROTOCOL_HEADER_SIZE];
    for (size_t received = 0; received < sizeof reply;) {
        ssize_t count = recv(fd, reply + received, sizeof reply - received, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            fputs("Could not read login response\n", stderr);
            return EXIT_FAILURE;
        }
        received += (size_t)count;
    }
    struct message_header response;
    protocol_decode_header(&response, reply);
    if (response.type != MSG_RESPONSE || response.length != 0 ||
        response.request_id != header.request_id) {
        fputs("Unexpected login response\n", stderr);
        return EXIT_FAILURE;
    }
    if (response.status != ERR_SUCCESS) {
        fprintf(stderr, "Login rejected (status 0x%02X)\n",
                (unsigned)response.status);
        return EXIT_FAILURE;
    }
    printf("Logged in as %s\n", username);
    return EXIT_SUCCESS;
}

int main(int argc, char *argv[])
{
    if (argc > 3) {
        fprintf(stderr, "Usage: %s [server-ipv4 [username]]\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *server_ip = argc >= 2 ? argv[1] : WEREWOLF_DEFAULT_ADDRESS;
    size_t username_length = argc == 3 ? strlen(argv[2]) : 0;
    if (argc == 3 && (username_length == 0 ||
                      username_length > PROTOCOL_MAX_USERNAME)) {
        fprintf(stderr, "Username must contain 1 to %d bytes\n",
                PROTOCOL_MAX_USERNAME);
        return EXIT_FAILURE;
    }
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(WEREWOLF_PORT),
    };
    if (inet_pton(AF_INET, server_ip, &address.sin_addr) != 1) {
        fprintf(stderr, "Invalid IPv4 address: %s\n", server_ip);
        return EXIT_FAILURE;
    }

    // Kết nối TCP trước khi gửi yêu cầu đăng nhập.
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    if (connect(server, (const struct sockaddr *)&address, sizeof address) == -1) {
        perror("connect");
        close(server);
        return EXIT_FAILURE;
    }

    printf("Connected to %s:%d\n", server_ip, WEREWOLF_PORT);
    int result = argc == 3 ? login(server, argv[2], username_length) : EXIT_SUCCESS;
    // Giữ kết nối để tiếp tục xử lý phòng chờ.
    if (argc == 3 && result == EXIT_SUCCESS) {
        result = run_lobby(server, argv[2]);
    }
    close(server);
    return result;
}
