#include "net_config.h"
#include "session.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [bind-ipv4]\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *bind_ip = argc == 2 ? argv[1] : WEREWOLF_DEFAULT_ADDRESS;
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(WEREWOLF_PORT),
    };
    if (inet_pton(AF_INET, bind_ip, &address.sin_addr) != 1) {
        fprintf(stderr, "Invalid IPv4 address: %s\n", bind_ip);
        return EXIT_FAILURE;
    }

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int reuse_address = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse_address,
                   sizeof reuse_address) == -1) {
        perror("setsockopt");
        close(listener);
        return EXIT_FAILURE;
    }

    if (bind(listener, (const struct sockaddr *)&address, sizeof address) == -1) {
        perror("bind");
        close(listener);
        return EXIT_FAILURE;
    }

    if (listen(listener, SOMAXCONN) == -1) {
        perror("listen");
        close(listener);
        return EXIT_FAILURE;
    }

    printf("Listening on %s:%d\n", bind_ip, WEREWOLF_PORT);
    fflush(stdout);
    int result = serve_sessions(listener);
    close(listener);
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
