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

static struct game_player *find_player_mutable(struct game *game,
                                               uint32_t player_id)
{
    for (size_t i = 0; i < game->player_count; ++i)
    {
        if (game->players[i].id == player_id)
        {
            return &game->players[i];
        }
    }
    return NULL;
}

static bool append_event(struct game_event_batch *events,
                         enum game_event_type type,
                         struct game_event **event)
{
    if (events->count >= GAME_MAX_EVENTS)
    {
        return false;
    }
    *event = &events->events[events->count++];
    **event = (struct game_event){0};
    (*event)->type = type;
    return true;
}

static void add_connected_recipients(const struct game *game,
                                     struct game_event *event)
{
    for (size_t i = 0; i < game->player_count; ++i)
    {
        if (game->players[i].connected)
        {
            event->recipient_ids[event->recipient_count++] =
                game->players[i].id;
        }
    }
}

static bool valid_night_target(const struct game_player *actor,
                               const struct game_player *target)
{
    if (target == NULL || !target->alive)
    {
        return false;
    }
    if (actor->role == GAME_ROLE_WEREWOLF)
    {
        return target->role != GAME_ROLE_WEREWOLF;
    }
    if (actor->role == GAME_ROLE_SEER)
    {
        return target->id != actor->id;
    }
    if (actor->role == GAME_ROLE_GUARD)
    {
        return target->id != actor->last_guarded_id;
    }
    return false;
}

static bool all_night_actions_submitted(const struct game *game)
{
    for (size_t i = 0; i < game->player_count; ++i)
    {
        const struct game_player *player = &game->players[i];
        if (!player->alive || !player->connected ||
            player->role == GAME_ROLE_VILLAGER)
        {
            continue;
        }
        const struct game_player *target =
            game_find_player(game, player->night_target_id);
        if (!player->night_action_submitted ||
            !valid_night_target(player, target))
        {
            return false;
        }
    }
    return true;
}

static void reset_round_data(struct game *game)
{
    for (size_t i = 0; i < game->player_count; ++i)
    {
        game->players[i].night_action_submitted = false;
        game->players[i].night_target_id = 0;
        game->players[i].vote_submitted = false;
        game->players[i].vote_target_id = 0;
    }
}

static bool append_night_result(const struct game *game, uint32_t killed_id,
                                struct game_event_batch *events)
{
    struct game_event *event;
    if (!append_event(events, GAME_EVENT_NIGHT_RESULT, &event))
    {
        return false;
    }
    add_connected_recipients(game, event);
    event->data.night_result.killed_player_id = killed_id;
    return true;
}

static bool append_day_discuss_event(const struct game *game,
                                     struct game_event_batch *events)
{
    struct game_event *event;
    if (!append_event(events, GAME_EVENT_PHASE_CHANGE, &event))
    {
        return false;
    }
    add_connected_recipients(game, event);
    event->data.phase_change.phase = GAME_PHASE_DAY_DISCUSS;
    event->data.phase_change.duration_sec = GAME_DAY_DISCUSS_DURATION_SEC;
    event->data.phase_change.deadline_ms = game->deadline_ms;
    return true;
}

/* Phân xử một đêm đúng một lần. SERVER-10 sẽ xử lý nhánh có người chết. */
static enum game_result settle_night(struct game *game, uint64_t now_ms,
                                     struct game_event_batch *events)
{
    if (game->awaiting_win_check)
    {
        return GAME_RESULT_OK;
    }

    uint8_t votes[PROTOCOL_MAX_PLAYERS] = {0};
    for (size_t i = 0; i < game->player_count; ++i)
    {
        const struct game_player *wolf = &game->players[i];
        if (wolf->alive && wolf->connected &&
            wolf->role == GAME_ROLE_WEREWOLF &&
            wolf->night_action_submitted)
        {
            const struct game_player *target =
                game_find_player(game, wolf->night_target_id);
            if (valid_night_target(wolf, target))
            {
                votes[(size_t)(target - game->players)]++;
            }
        }
    }

    uint8_t highest = 0;
    size_t chosen_index = PROTOCOL_MAX_PLAYERS;
    bool tied = false;
    for (size_t i = 0; i < game->player_count; ++i)
    {
        if (votes[i] > highest)
        {
            highest = votes[i];
            chosen_index = i;
            tied = false;
        }
        else if (votes[i] != 0 && votes[i] == highest)
        {
            tied = true;
        }
    }

    uint32_t killed_id = 0;
    if (highest != 0 && !tied)
    {
        uint32_t chosen_id = game->players[chosen_index].id;
        bool protected_target = false;
        for (size_t i = 0; i < game->player_count; ++i)
        {
            const struct game_player *guard = &game->players[i];
            if (guard->alive && guard->connected &&
                guard->role == GAME_ROLE_GUARD &&
                guard->night_action_submitted &&
                guard->night_target_id == chosen_id)
            {
                protected_target = true;
                break;
            }
        }
        if (!protected_target)
        {
            killed_id = chosen_id;
        }
    }

    if (killed_id == 0 &&
        now_ms > UINT64_MAX -
                     (uint64_t)GAME_DAY_DISCUSS_DURATION_SEC * UINT64_C(1000))
    {
        return GAME_RESULT_INTERNAL;
    }

    struct game next_game = *game;
    struct game_event_batch next_events = *events;
    if (killed_id != 0)
    {
        struct game_player *killed =
            find_player_mutable(&next_game, killed_id);
        killed->alive = false;
    }

    /* Giữ mục tiêu Bảo vệ nếu đã hành động, bỏ lượt thì xóa lịch sử. */
    for (size_t i = 0; i < next_game.player_count; ++i)
    {
        struct game_player *guard = &next_game.players[i];
        if (guard->role == GAME_ROLE_GUARD)
        {
            guard->last_guarded_id =
                guard->night_action_submitted
                    ? guard->night_target_id
                    : 0;
        }
    }

    reset_round_data(&next_game);
    if (!append_night_result(&next_game, killed_id, &next_events))
    {
        return GAME_RESULT_INTERNAL;
    }

    if (killed_id != 0)
    {
        next_game.awaiting_win_check = true;
    }
    else
    {
        next_game.phase = GAME_PHASE_DAY_DISCUSS;
        next_game.phase_started_ms = now_ms;
        next_game.deadline_ms =
            now_ms +
            (uint64_t)GAME_DAY_DISCUSS_DURATION_SEC * UINT64_C(1000);
        if (!append_day_discuss_event(&next_game, &next_events))
        {
            return GAME_RESULT_INTERNAL;
        }
    }

    *game = next_game;
    *events = next_events;
    return GAME_RESULT_OK;
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
        game->phase == GAME_PHASE_NONE || game->awaiting_win_check)
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

enum game_result game_handle_command(struct game *game, uint32_t player_id,
                                     uint64_t now_ms,
                                     const struct game_command *command,
                                     struct game_event_batch *events)
{
    if (events != NULL)
    {
        events->count = 0;
    }
    if (game == NULL || events == NULL || command == NULL ||
        !game->initialized)
    {
        return GAME_RESULT_INTERNAL;
    }
    if (command->type == GAME_COMMAND_CHAT ||
        command->type == GAME_COMMAND_DAY_VOTE)
    {
        /* SERVER-09 chưa triển khai trong mô-đun này. */
        return GAME_RESULT_INTERNAL;
    }
    if (command->type != GAME_COMMAND_NIGHT_ACTION)
    {
        return GAME_RESULT_INVALID_PAYLOAD;
    }
    if (game_is_over(game) || game->phase != GAME_PHASE_NIGHT ||
        game->awaiting_win_check)
    {
        return GAME_RESULT_WRONG_PHASE;
    }

    /* Hết hạn được chốt trước khi xét lệnh đến trễ. */
    if (now_ms >= game->deadline_ms)
    {
        enum game_result timeout_result =
            game_handle_timeout(game, now_ms, events);
        if (timeout_result != GAME_RESULT_OK)
        {
            return timeout_result;
        }
        return GAME_RESULT_WRONG_PHASE;
    }

    struct game_player *actor = find_player_mutable(game, player_id);
    if (actor == NULL || !actor->alive || !actor->connected ||
        actor->role == GAME_ROLE_VILLAGER)
    {
        return GAME_RESULT_UNAUTHORIZED;
    }

    struct game_player *target = find_player_mutable(
        game, command->data.night_action.target_player_id);
    if (!valid_night_target(actor, target))
    {
        return GAME_RESULT_INVALID_TARGET;
    }
    if (actor->night_action_submitted &&
        actor->role != GAME_ROLE_WEREWOLF)
    {
        return GAME_RESULT_UNAUTHORIZED;
    }

    struct game next_game = *game;
    struct game_event_batch next_events = {0};
    struct game_player *next_actor =
        find_player_mutable(&next_game, player_id);
    const struct game_player *next_target = game_find_player(
        &next_game, command->data.night_action.target_player_id);
    next_actor->night_action_submitted = true;
    next_actor->night_target_id = next_target->id;

    if (next_actor->role == GAME_ROLE_SEER)
    {
        struct game_event *event;
        if (!append_event(&next_events, GAME_EVENT_SEER_RESULT, &event))
        {
            return GAME_RESULT_INTERNAL;
        }
        event->recipient_count = 1;
        event->recipient_ids[0] = next_actor->id;
        event->data.seer_result.target_player_id = next_target->id;
        event->data.seer_result.is_werewolf =
            next_target->role == GAME_ROLE_WEREWOLF;
    }
    else if (next_actor->role == GAME_ROLE_WEREWOLF)
    {
        struct game_event *event = NULL;
        for (size_t i = 0; i < next_game.player_count; ++i)
        {
            const struct game_player *wolf = &next_game.players[i];
            if (wolf->id == next_actor->id || !wolf->alive ||
                !wolf->connected || wolf->role != GAME_ROLE_WEREWOLF)
            {
                continue;
            }
            if (event == NULL &&
                !append_event(&next_events,
                              GAME_EVENT_WOLF_TARGET_UPDATE, &event))
            {
                return GAME_RESULT_INTERNAL;
            }
            event->recipient_ids[event->recipient_count++] = wolf->id;
        }
        if (event != NULL)
        {
            event->data.wolf_target_update.wolf_id = next_actor->id;
            event->data.wolf_target_update.target_player_id = next_target->id;
        }
    }

    if (all_night_actions_submitted(&next_game))
    {
        enum game_result settle_result =
            settle_night(&next_game, now_ms, &next_events);
        if (settle_result != GAME_RESULT_OK)
        {
            return settle_result;
        }
    }

    *game = next_game;
    *events = next_events;
    return GAME_RESULT_OK;
}

enum game_result game_handle_timeout(struct game *game, uint64_t now_ms,
                                     struct game_event_batch *events)
{
    if (events != NULL)
    {
        events->count = 0;
    }
    if (game == NULL || events == NULL || !game->initialized)
    {
        return GAME_RESULT_INTERNAL;
    }
    if (game_is_over(game) || game->awaiting_win_check)
    {
        return GAME_RESULT_OK;
    }
    if (now_ms < game->deadline_ms)
    {
        return GAME_RESULT_OK;
    }
    if (game->phase != GAME_PHASE_NIGHT)
    {
        /* Timeout pha ngày thuộc SERVER-09. */
        return GAME_RESULT_INTERNAL;
    }
    return settle_night(game, now_ms, events);
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
