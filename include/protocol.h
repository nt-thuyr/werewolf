#ifndef WEREWOLF_PROTOCOL_H
#define WEREWOLF_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    PROTOCOL_HEADER_SIZE = 8,
    PROTOCOL_MAX_BODY = 4096,
    PROTOCOL_MAX_FRAME = PROTOCOL_HEADER_SIZE + PROTOCOL_MAX_BODY,
    PROTOCOL_MAX_NAME = 32,
    PROTOCOL_MAX_USERNAME = PROTOCOL_MAX_NAME,
    PROTOCOL_MIN_PLAYERS = 5,
    PROTOCOL_MAX_PLAYERS = 12
};

enum {
    MSG_LOGIN_REQ = 0x01,
    MSG_ROOM_LIST_REQ = 0x02,
    MSG_ROOM_LIST_RESP = 0x03,
    MSG_CREATE_ROOM_REQ = 0x04,
    MSG_JOIN_ROOM_REQ = 0x05,
    MSG_LEAVE_ROOM_REQ = 0x06,
    MSG_ROOM_UPDATE = 0x07,
    MSG_INVITE_REQ = 0x10,
    MSG_INVITE_NOTIF = 0x11,
    MSG_KICK_REQ = 0x12,
    MSG_KICK_NOTIF = 0x13,
    MSG_RESPONSE = 0xFF
};

enum {
    KICK_REASON_HOST = 0x01
};

enum {
    ERR_SUCCESS = 0x00,
    ERR_UNAUTHENTICATED = 0x01,
    ERR_UNAUTHORIZED = 0x02,
    ERR_INVALID_TARGET = 0x04,
    ERR_ROOM_FULL = 0x05,
    ERR_USERNAME_EXIST = 0x06,
    ERR_INVALID_PAYLOAD = 0x08,
    ERR_ROOM_NOT_FOUND = 0x09,
    ERR_INTERNAL = 0x0C
};

// Chỉ dùng trong bộ nhớ. Header trên TCP được mã hóa từng trường.
struct message_header {
    uint8_t type;
    uint16_t length;
    uint32_t request_id;
    uint8_t status;
};

// Cần đủ PROTOCOL_HEADER_SIZE byte. Bên gọi kiểm tra tính hợp lệ của khung.
void protocol_encode_header(uint8_t *bytes, const struct message_header *header);
void protocol_decode_header(struct message_header *header, const uint8_t *bytes);

// Bên gọi bảo đảm vùng đọc hoặc ghi có đủ 2 hay 4 byte tương ứng.
void protocol_put_u16(uint8_t *bytes, uint16_t value);
void protocol_put_u32(uint8_t *bytes, uint32_t value);
uint16_t protocol_get_u16(const uint8_t *bytes);
uint32_t protocol_get_u32(const uint8_t *bytes);

// name dài 1–PROTOCOL_MAX_NAME byte. bytes cần đủ 1 + strlen(name) byte.
size_t protocol_put_name(uint8_t *bytes, const char *name);

// Khi đọc thành công, thêm NUL vào name và tăng offset qua trường Name.
// Trả false nếu trường Name không hợp lệ, giữ nguyên offset và name.
bool protocol_get_name(const uint8_t *body, size_t length, size_t *offset,
                       char name[PROTOCOL_MAX_NAME + 1]);

#endif
