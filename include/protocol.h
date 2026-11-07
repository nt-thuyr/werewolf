#ifndef WEREWOLF_PROTOCOL_H
#define WEREWOLF_PROTOCOL_H

#include <stdint.h>

enum {
    PROTOCOL_HEADER_SIZE = 8,
    PROTOCOL_MAX_BODY = 4096,
    PROTOCOL_MAX_USERNAME = 32
};

enum {
    MSG_LOGIN_REQ = 0x01,
    MSG_RESPONSE = 0xFF
};

enum {
    ERR_SUCCESS = 0x00,
    ERR_UNAUTHORIZED = 0x02,
    ERR_USERNAME_EXIST = 0x06,
    ERR_INVALID_PAYLOAD = 0x08
};

struct message_header {
    uint8_t type;
    uint16_t length;
    uint32_t request_id;
    uint8_t status;
};

void protocol_encode_header(uint8_t *bytes, const struct message_header *header);
void protocol_decode_header(struct message_header *header, const uint8_t *bytes);

#endif
