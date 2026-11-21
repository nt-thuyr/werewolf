#include "server/game.h"

#include <limits.h>

static uint8_t wolf_count_for(size_t player_count)
{
    if (player_count >= 10)
    {
        return 3;
    }
    if (player_count >= 7)
    {
        return 2;
    }
    return 1;
}

/* Bộ sinh số giả ngẫu nhiên nhỏ, chỉ dùng để chia vai lặp lại theo seed. */
static uint64_t next_random(uint64_t *state)
{
    uint64_t value = *state;
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    *state = value;
    return value * UINT64_C(2685821657736338717);
}

/* Lấy số trong [0, upper_bound) mà không tạo lệch do phép chia dư. */
static uint64_t random_below(uint64_t *state, uint64_t upper_bound)
{
    uint64_t threshold = (UINT64_C(0) - upper_bound) % upper_bound;
    uint64_t value;
    do
    {
        value = next_random(state);
    } while (value < threshold);
    return value % upper_bound;
}

static void shuffle_roles(enum game_role *roles, size_t count,
                          uint64_t *random_state)
{
    for (size_t i = count; i > 1; --i)
    {
        size_t other = (size_t)random_below(random_state, i);
        enum game_role temporary = roles[i - 1];
        roles[i - 1] = roles[other];
        roles[other] = temporary;
    }
}

static bool valid_player_ids(const uint32_t *player_ids, size_t player_count)
{
    for (size_t i = 0; i < player_count; ++i)
    {
        if (player_ids[i] == 0)
        {
            return false;
        }
        for (size_t j = 0; j < i; ++j)
        {
            if (player_ids[i] == player_ids[j])
            {
                return false;
            }
        }
    }
    return true;
}

static bool append_role_events(const struct game *game,
                               struct game_event_batch *events)
{
    if (events->count + game->player_count > GAME_MAX_EVENTS)
    {
        return false;
    }

    for (size_t i = 0; i < game->player_count; ++i)
    {
        struct game_event *event = &events->events[events->count++];
        *event = (struct game_event){0};
        event->type = GAME_EVENT_ROLE_ASSIGN;
        event->recipient_count = 1;
        event->recipient_ids[0] = game->players[i].id;
        event->data.role_assign.role = game->players[i].role;
    }
    return true;
}

static bool append_wolf_team_event(const struct game *game,
                                   struct game_event_batch *events)
{
    if (events->count == GAME_MAX_EVENTS)
    {
        return false;
    }

    struct game_event *event = &events->events[events->count++];
    *event = (struct game_event){0};
    event->type = GAME_EVENT_WOLF_TEAM_INFO;

    for (size_t i = 0; i < game->player_count; ++i)
    {
        if (game->players[i].role != GAME_ROLE_WEREWOLF)
        {
            continue;
        }

        uint8_t index = event->data.wolf_team_info.wolf_count;
        if (index >= PROTOCOL_MAX_PLAYERS)
        {
            return false;
        }
        event->recipient_ids[index] = game->players[i].id;
        event->data.wolf_team_info.wolf_ids[index] = game->players[i].id;
        event->data.wolf_team_info.wolf_count++;
    }

    event->recipient_count = event->data.wolf_team_info.wolf_count;
    return event->recipient_count != 0;
}

static bool append_first_phase_event(const struct game *game,
                                     struct game_event_batch *events)
{
    if (events->count == GAME_MAX_EVENTS)
    {
        return false;
    }

    struct game_event *event = &events->events[events->count++];
    *event = (struct game_event){0};
    event->type = GAME_EVENT_PHASE_CHANGE;
    event->recipient_count = game->player_count;
    for (size_t i = 0; i < game->player_count; ++i)
    {
        event->recipient_ids[i] = game->players[i].id;
    }
    event->data.phase_change.phase = GAME_PHASE_NIGHT;
    event->data.phase_change.duration_sec = GAME_NIGHT_DURATION_SEC;
    event->data.phase_change.deadline_ms = game->deadline_ms;
    return true;
}

const struct game_player *game_find_player(const struct game *game,
                                           uint32_t player_id)
{
    if (game == NULL || !game->initialized || player_id == 0)
    {
        return NULL;
    }

    for (size_t i = 0; i < game->player_count; ++i)
    {
        if (game->players[i].id == player_id)
        {
            return &game->players[i];
        }
    }
    return NULL;
}

uint64_t game_next_deadline_ms(const struct game *game)
{
    if (game == NULL || !game->initialized || game_is_over(game) ||
        game->phase == GAME_PHASE_NONE)
    {
        return UINT64_MAX;
    }
    return game->deadline_ms;
}

bool game_is_over(const struct game *game)
{
    return game != NULL && game->initialized &&
           game->phase == GAME_PHASE_OVER;
}

enum game_result game_init(struct game *game,
                           const struct game_start_input *input,
                           struct game_event_batch *events)
{
    if (events != NULL)
    {
        events->count = 0;
    }
    if (game == NULL || events == NULL)
    {
        return GAME_RESULT_INTERNAL;
    }
    if (input == NULL || input->player_ids == NULL || input->random_seed == 0)
    {
        return GAME_RESULT_INVALID_PAYLOAD;
    }
    if (input->player_count < PROTOCOL_MIN_PLAYERS)
    {
        return GAME_RESULT_NOT_ENOUGH_PLAYERS;
    }
    if (input->player_count > PROTOCOL_MAX_PLAYERS ||
        !valid_player_ids(input->player_ids, input->player_count))
    {
        return GAME_RESULT_INVALID_PAYLOAD;
    }

    const uint64_t night_duration_ms =
        (uint64_t)GAME_NIGHT_DURATION_SEC * UINT64_C(1000);
    if (input->now_ms > UINT64_MAX - night_duration_ms)
    {
        return GAME_RESULT_INVALID_PAYLOAD;
    }

    /* Dựng vào biến tạm để lỗi không làm ván hiện tại bị dở dang. */
    struct game next_game = {0};
    struct game_event_batch next_events = {0};
    next_game.initialized = true;
    next_game.phase = GAME_PHASE_NIGHT;
    next_game.winner = GAME_FACTION_NONE;
    next_game.player_count = (uint8_t)input->player_count;
    next_game.round_number = 1;
    next_game.phase_started_ms = input->now_ms;
    next_game.deadline_ms = input->now_ms + night_duration_ms;
    next_game.random_state = input->random_seed;

    enum game_role roles[PROTOCOL_MAX_PLAYERS];
    uint8_t wolf_count = wolf_count_for(input->player_count);
    size_t role_index = 0;
    for (uint8_t i = 0; i < wolf_count; ++i)
    {
        roles[role_index++] = GAME_ROLE_WEREWOLF;
    }
    roles[role_index++] = GAME_ROLE_SEER;
    roles[role_index++] = GAME_ROLE_GUARD;
    while (role_index < input->player_count)
    {
        roles[role_index++] = GAME_ROLE_VILLAGER;
    }
    shuffle_roles(roles, input->player_count, &next_game.random_state);

    for (size_t i = 0; i < input->player_count; ++i)
    {
        next_game.players[i] = (struct game_player){
            .id = input->player_ids[i],
            .role = roles[i],
            .alive = true,
            .connected = true
        };
    }

    if (!append_role_events(&next_game, &next_events) ||
        !append_wolf_team_event(&next_game, &next_events) ||
        !append_first_phase_event(&next_game, &next_events))
    {
        return GAME_RESULT_INTERNAL;
    }

    *game = next_game;
    *events = next_events;
    return GAME_RESULT_OK;
}

/* Các hàm dưới đây sẽ được triển khai ở các task luật chơi tiếp theo. */
enum game_result game_handle_command(struct game *game, uint32_t player_id,
                                     uint64_t now_ms,
                                     const struct game_command *command,
                                     struct game_event_batch *events)
{
    (void)game;
    (void)player_id;
    (void)now_ms;
    (void)command;
    if (events != NULL)
    {
        events->count = 0;
    }
    return GAME_RESULT_INTERNAL;
}

enum game_result game_handle_timeout(struct game *game, uint64_t now_ms,
                                     struct game_event_batch *events)
{
    (void)game;
    (void)now_ms;
    if (events != NULL)
    {
        events->count = 0;
    }
    return GAME_RESULT_INTERNAL;
}

enum game_result game_handle_player_left(struct game *game,
                                         uint32_t player_id, uint64_t now_ms,
                                         struct game_event_batch *events)
{
    (void)game;
    (void)player_id;
    (void)now_ms;
    if (events != NULL)
    {
        events->count = 0;
    }
    return GAME_RESULT_INTERNAL;
}
