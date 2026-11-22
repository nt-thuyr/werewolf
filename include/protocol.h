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
    PROTOCOL_MAX_CHAT_BYTES = 512,
    PROTOCOL_MAX_SUMMARY_BYTES = 256,
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
    MSG_START_GAME_REQ = 0x14,
    MSG_ROLE_ASSIGN = 0x20,
    MSG_WOLF_TEAM_INFO = 0x22,
    MSG_PHASE_CHANGE = 0x30,
    MSG_NIGHT_ACTION_REQ = 0x31,
    MSG_SEER_RESULT = 0x32,
    MSG_WOLF_TARGET_UPDATE = 0x33,
    MSG_NIGHT_RESULT = 0x34,
    MSG_DAY_VOTE_REQ = 0x35,
    MSG_DAY_RESULT = 0x36,
    MSG_CHAT_MSG = 0x37,
    MSG_GAME_OVER = 0x38,
    MSG_CHAT_NOTIF = 0x39,
    MSG_RESPONSE = 0xFF
};

enum protocol_role {
    PROTOCOL_ROLE_WEREWOLF = 1,
    PROTOCOL_ROLE_VILLAGER = 2,
    PROTOCOL_ROLE_SEER = 3,
    PROTOCOL_ROLE_GUARD = 4
};

enum protocol_phase {
    PROTOCOL_PHASE_NIGHT = 1,
    PROTOCOL_PHASE_DAY_DISCUSS = 2,
    PROTOCOL_PHASE_DAY_VOTE = 3
};

enum protocol_faction {
    PROTOCOL_FACTION_NONE = 0,
    PROTOCOL_FACTION_VILLAGE = 1,
    PROTOCOL_FACTION_WEREWOLF = 2
};

enum protocol_chat_scope {
    PROTOCOL_CHAT_VILLAGE = 0,
    PROTOCOL_CHAT_WEREWOLF = 1
};

enum {
    KICK_REASON_HOST = 0x01
};

enum {
    ERR_SUCCESS = 0x00,
    ERR_UNAUTHENTICATED = 0x01,
    ERR_UNAUTHORIZED = 0x02,
    ERR_WRONG_PHASE = 0x03,
    ERR_INVALID_TARGET = 0x04,
    ERR_ROOM_FULL = 0x05,
    ERR_USERNAME_EXIST = 0x06,
    ERR_GAME_RUNNING = 0x07,
    ERR_INVALID_PAYLOAD = 0x08,
    ERR_ROOM_NOT_FOUND = 0x09,
    ERR_NOT_ENOUGH_PLAYERS = 0x0A,
    ERR_RATE_LIMITED = 0x0B,
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

// Bên gọi bảo đảm vùng đọc hoặc ghi có đủ 2, 4 hoặc 8 byte tương ứng.
void protocol_put_u16(uint8_t *bytes, uint16_t value);
void protocol_put_u32(uint8_t *bytes, uint32_t value);
void protocol_put_u64(uint8_t *bytes, uint64_t value);
uint16_t protocol_get_u16(const uint8_t *bytes);
uint32_t protocol_get_u32(const uint8_t *bytes);
uint64_t protocol_get_u64(const uint8_t *bytes);

// name dài 1–PROTOCOL_MAX_NAME byte. bytes cần đủ 1 + strlen(name) byte.
size_t protocol_put_name(uint8_t *bytes, const char *name);

// Khi đọc thành công, thêm NUL vào name và tăng offset qua trường Name.
// Trả false nếu trường Name không hợp lệ, giữ nguyên offset và name.
bool protocol_get_name(const uint8_t *body, size_t length, size_t *offset,
                       char name[PROTOCOL_MAX_NAME + 1]);

#endif
