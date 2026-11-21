#ifndef WEREWOLF_SERVER_GAME_H
#define WEREWOLF_SERVER_GAME_H

#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Mô-đun game chỉ quản lý luật chơi và trạng thái riêng của từng ván.
 * Không quản lý socket, phiên kết nối hay phòng.
 *
 * Đầu vào: ID người chơi và thời gian monotonic.
 * Đầu ra: các sự kiện kèm ID người nhận.
 *
 * Lớp tích hợp với server để xử lý MSG_RESPONSE,
 * request_id và mã hóa dữ liệu để truyền qua mạng.
 */

// Thời lượng và giới hạn dữ liệu của game
enum
{
    GAME_NIGHT_DURATION_SEC = 30,
    GAME_DAY_DISCUSS_DURATION_SEC = 90,
    GAME_DAY_VOTE_DURATION_SEC = 30,
    GAME_MAX_CHAT_BYTES = 512,
    GAME_MAX_SUMMARY_BYTES = 256,
    GAME_MAX_EVENTS = 32
};

// Các vai trò của người chơi trong ván
enum game_role
{
    GAME_ROLE_NONE = 0,
    GAME_ROLE_WEREWOLF = 1,
    GAME_ROLE_VILLAGER = 2,
    GAME_ROLE_SEER = 3,
    GAME_ROLE_GUARD = 4
};

/* 1-3 trùng phase_id của MSG_PHASE_CHANGE */
enum game_phase
{
    GAME_PHASE_NONE = 0,
    GAME_PHASE_NIGHT = 1,
    GAME_PHASE_DAY_DISCUSS = 2,
    GAME_PHASE_DAY_VOTE = 3,
    GAME_PHASE_OVER = 4
};

/* 1-2 trùng winner_faction của MSG_GAME_OVER. */
enum game_faction
{
    GAME_FACTION_NONE = 0,
    GAME_FACTION_VILLAGE = 1,
    GAME_FACTION_WEREWOLF = 2
};

enum game_chat_scope
{
    GAME_CHAT_VILLAGE = 0,
    GAME_CHAT_WEREWOLF = 1
};

/* status_code của MSG_RESPONSE */
enum game_result
{
    GAME_RESULT_OK = ERR_SUCCESS,
    GAME_RESULT_UNAUTHORIZED = ERR_UNAUTHORIZED,
    GAME_RESULT_WRONG_PHASE = ERR_WRONG_PHASE,
    GAME_RESULT_INVALID_TARGET = ERR_INVALID_TARGET,
    GAME_RESULT_INVALID_PAYLOAD = ERR_INVALID_PAYLOAD,
    GAME_RESULT_NOT_ENOUGH_PLAYERS = ERR_NOT_ENOUGH_PLAYERS,
    GAME_RESULT_INTERNAL = ERR_INTERNAL
};

struct game_player
{
    uint32_t id;
    enum game_role role;
    bool alive;
    bool connected;

    /* Hành động trong lượt hiện tại*/
    bool night_action_submitted;
    uint32_t night_target_id;
    bool vote_submitted;
    uint32_t vote_target_id; /* 0 : phiếu trắng */

    /* Mục tiêu Bảo vệ của đêm trước */
    uint32_t last_guarded_id;
};

struct game
{
    bool initialized;
    enum game_phase phase;
    enum game_faction winner;
    uint8_t player_count;
    uint32_t round_number;
    uint64_t phase_started_ms;
    uint64_t deadline_ms;
    uint64_t random_state;
    struct game_player players[PROTOCOL_MAX_PLAYERS];
};

/* player_ids chỉ được đọc trong game_init() */
struct game_start_input
{
    const uint32_t *player_ids;
    size_t player_count;
    uint64_t now_ms;

    /* Seed khác 0 để kiểm thử chia vai lặp lại được */
    uint64_t random_seed;
};

enum game_command_type
{
    GAME_COMMAND_NONE = 0,
    GAME_COMMAND_NIGHT_ACTION,
    GAME_COMMAND_CHAT,
    GAME_COMMAND_DAY_VOTE
};

struct game_command
{
    enum game_command_type type;
    union
    {
        struct
        {
            uint32_t target_player_id;
        } night_action;
        struct
        {
            enum game_chat_scope scope;
            const uint8_t *message;
            size_t message_length;
        } chat;
        struct
        {
            uint32_t target_player_id; /* 0: phiếu trắng */
        } day_vote;
    } data;
};

enum game_event_type
{
    GAME_EVENT_ROLE_ASSIGN,
    GAME_EVENT_WOLF_TEAM_INFO,
    GAME_EVENT_PHASE_CHANGE,
    GAME_EVENT_SEER_RESULT,
    GAME_EVENT_WOLF_TARGET_UPDATE,
    GAME_EVENT_NIGHT_RESULT,
    GAME_EVENT_DAY_RESULT,
    GAME_EVENT_CHAT,
    GAME_EVENT_GAME_OVER
};

struct game_event
{
    enum game_event_type type;

    /*
     * Danh sách người nhận không trùng nhau. Sự kiện chung gửi
    cho mọi người đag kết nối;
     * sự kiện riêng chỉ gửi đúng vai được phép.
     */
    uint8_t recipient_count;
    uint32_t recipient_ids[PROTOCOL_MAX_PLAYERS];

    union
    {
        struct
        {
            enum game_role role;
        } role_assign;
        struct
        {
            uint8_t wolf_count;
            uint32_t wolf_ids[PROTOCOL_MAX_PLAYERS];
        } wolf_team_info;
        struct
        {
            enum game_phase phase;
            uint16_t duration_sec;
            uint64_t deadline_ms;
        } phase_change;
        struct
        {
            uint32_t target_player_id;
            bool is_werewolf;
        } seer_result;
        struct
        {
            uint32_t wolf_id;
            uint32_t target_player_id;
        } wolf_target_update;
        struct
        {
            uint32_t killed_player_id; /* 0: không ai chết */
        } night_result;
        struct
        {
            uint32_t executed_player_id; /* 0: không ai bị treo */
        } day_result;
        struct
        {
            enum game_chat_scope scope;
            uint32_t from_player_id;
            uint16_t message_length;
            uint8_t message[GAME_MAX_CHAT_BYTES];
        } chat;
        struct
        {
            enum game_faction winner;
            uint16_t summary_length;
            uint8_t summary[GAME_MAX_SUMMARY_BYTES];
        } game_over;
    } data;
};

/* Các hàm xử lý xóa count trước khi thêm sự kiện. Bên gọi sở hữu batch. */
struct game_event_batch
{
    size_t count;
    struct game_event events[GAME_MAX_EVENTS];
};

/*
 * Tạo ván, chia vai và mở pha Đêm. Thành công thì trả sự kiện vai riêng,
 * danh sách Sói và sự kiện đổi pha chung. Cần 5-12 ID khác 0, không trùng.
 */
enum game_result game_init(struct game *game,
                           const struct game_start_input *input,
                           struct game_event_batch *events);

/*
 * Xử lý lệnh của player_id tại now_ms. Game kiểm tra hạn trước nên lệnh trễ có
 * thể trả lỗi và vẫn sinh sự kiện chốt pha. Luôn gửi MSG_RESPONSE trước sự kiện.
 */
enum game_result game_handle_command(struct game *game, uint32_t player_id,
                                     uint64_t now_ms,
                                     const struct game_command *command,
                                     struct game_event_batch *events);

/*
 * Chốt pha nếu đã đến hạn. Pha mới bắt đầu tại now_ms và có hạn mới.
 * Gọi trước hạn hoặc sau khi ván kết thúc thì không đổi trạng thái.
 */
enum game_result game_handle_timeout(struct game *game, uint64_t now_ms,
                                     struct game_event_batch *events);

/*
 * Đánh dấu người chơi đã rời và chết, bỏ hành động/phiếu rồi xét thắng.
 * Nếu ván tiếp tục thì giữ hạn pha. Lobby tự xử lý phòng và Chủ phòng.
 */
enum game_result game_handle_player_left(struct game *game,
                                         uint32_t player_id, uint64_t now_ms,
                                         struct game_event_batch *events);

/*
 * Xét thắng từ trạng thái sống/chết hiện tại. Hàm nối GAME_OVER vào batch đang
 * có để giữ thứ tự sau NIGHT_RESULT hoặc DAY_RESULT; không xóa events->count.
 */
enum game_result game_check_winner(struct game *game,
                                   struct game_event_batch *events);

/* Hàm chỉ đọc cho kiểm thử. */
const struct game_player *game_find_player(const struct game *game,
                                           uint32_t player_id);
/* Trả UINT64_MAX nếu không có pha đang chạy. */
uint64_t game_next_deadline_ms(const struct game *game);
bool game_is_over(const struct game *game);

#endif
