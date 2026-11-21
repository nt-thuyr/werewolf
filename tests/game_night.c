#include "server/game.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                     \
    do                                                                       \
    {                                                                        \
        if (!(condition))                                                    \
        {                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,          \
                    #condition);                                             \
            return false;                                                    \
        }                                                                    \
    } while (0)

static bool init_game(struct game *game, size_t player_count, uint32_t base_id,
                      uint64_t now_ms, uint64_t seed)
{
    uint32_t ids[PROTOCOL_MAX_PLAYERS];
    for (size_t i = 0; i < player_count; ++i)
    {
        ids[i] = base_id + (uint32_t)i;
    }
    struct game_start_input input = {
        .player_ids = ids,
        .player_count = player_count,
        .now_ms = now_ms,
        .random_seed = seed
    };
    struct game_event_batch events = {0};
    return game_init(game, &input, &events) == GAME_RESULT_OK;
}

static struct game_player *player_with_role(struct game *game,
                                            enum game_role role,
                                            size_t occurrence)
{
    for (size_t i = 0; i < game->player_count; ++i)
    {
        if (game->players[i].role == role)
        {
            if (occurrence == 0)
            {
                return &game->players[i];
            }
            --occurrence;
        }
    }
    return NULL;
}

static enum game_result night_action(struct game *game, uint32_t actor_id,
                                     uint32_t target_id, uint64_t now_ms,
                                     struct game_event_batch *events)
{
    const struct game_command command = {
        .type = GAME_COMMAND_NIGHT_ACTION,
        .data.night_action.target_player_id = target_id
    };
    return game_handle_command(game, actor_id, now_ms, &command, events);
}

static const struct game_event *find_event(
    const struct game_event_batch *events, enum game_event_type type,
    size_t occurrence)
{
    for (size_t i = 0; i < events->count; ++i)
    {
        if (events->events[i].type == type)
        {
            if (occurrence == 0)
            {
                return &events->events[i];
            }
            --occurrence;
        }
    }
    return NULL;
}

static bool has_recipient(const struct game_event *event, uint32_t player_id)
{
    for (size_t i = 0; i < event->recipient_count; ++i)
    {
        if (event->recipient_ids[i] == player_id)
        {
            return true;
        }
    }
    return false;
}

static bool test_valid_actions_and_private_events(void)
{
    struct game game = {0};
    struct game_event_batch events = {0};
    CHECK(init_game(&game, 7, 100, 1000, 11));

    struct game_player *wolf_a =
        player_with_role(&game, GAME_ROLE_WEREWOLF, 0);
    struct game_player *wolf_b =
        player_with_role(&game, GAME_ROLE_WEREWOLF, 1);
    struct game_player *seer = player_with_role(&game, GAME_ROLE_SEER, 0);
    struct game_player *guard = player_with_role(&game, GAME_ROLE_GUARD, 0);
    struct game_player *villager_a =
        player_with_role(&game, GAME_ROLE_VILLAGER, 0);
    struct game_player *villager_b =
        player_with_role(&game, GAME_ROLE_VILLAGER, 1);
    CHECK(wolf_a && wolf_b && seer && guard && villager_a && villager_b);

    CHECK(night_action(&game, wolf_a->id, villager_a->id, 1100, &events) ==
          GAME_RESULT_OK);
    const struct game_event *wolf_update =
        find_event(&events, GAME_EVENT_WOLF_TARGET_UPDATE, 0);
    CHECK(wolf_update != NULL);
    CHECK(wolf_update->recipient_count == 1);
    CHECK(has_recipient(wolf_update, wolf_b->id));
    CHECK(!has_recipient(wolf_update, wolf_a->id));
    CHECK(wolf_update->data.wolf_target_update.wolf_id == wolf_a->id);
    CHECK(wolf_update->data.wolf_target_update.target_player_id ==
          villager_a->id);

    /* Sói được đổi mục tiêu khi đêm còn mở. */
    CHECK(night_action(&game, wolf_a->id, villager_b->id, 1200, &events) ==
          GAME_RESULT_OK);
    wolf_update = find_event(&events, GAME_EVENT_WOLF_TARGET_UPDATE, 0);
    CHECK(wolf_update != NULL);
    CHECK(wolf_update->data.wolf_target_update.target_player_id ==
          villager_b->id);
    CHECK(wolf_a->night_target_id == villager_b->id);

    CHECK(night_action(&game, seer->id, wolf_a->id, 1300, &events) ==
          GAME_RESULT_OK);
    const struct game_event *seer_result =
        find_event(&events, GAME_EVENT_SEER_RESULT, 0);
    CHECK(seer_result != NULL);
    CHECK(seer_result->recipient_count == 1);
    CHECK(seer_result->recipient_ids[0] == seer->id);
    CHECK(seer_result->data.seer_result.target_player_id == wolf_a->id);
    CHECK(seer_result->data.seer_result.is_werewolf);
    CHECK(!has_recipient(seer_result, guard->id));

    CHECK(night_action(&game, guard->id, villager_b->id, 1400, &events) ==
          GAME_RESULT_OK);
    CHECK(events.count == 0);

    /* Sói cuối cùng hoàn tất đêm; Bảo vệ cứu đúng mục tiêu. */
    CHECK(night_action(&game, wolf_b->id, villager_b->id, 1500, &events) ==
          GAME_RESULT_OK);
    const struct game_event *night_result =
        find_event(&events, GAME_EVENT_NIGHT_RESULT, 0);
    const struct game_event *phase_change =
        find_event(&events, GAME_EVENT_PHASE_CHANGE, 0);
    CHECK(night_result != NULL);
    CHECK(night_result->data.night_result.killed_player_id == 0);
    CHECK(night_result->recipient_count == game.player_count);
    CHECK(phase_change != NULL);
    CHECK(phase_change->data.phase_change.phase == GAME_PHASE_DAY_DISCUSS);
    CHECK(phase_change->data.phase_change.duration_sec ==
          GAME_DAY_DISCUSS_DURATION_SEC);
    CHECK(phase_change->data.phase_change.deadline_ms == UINT64_C(91500));
    CHECK(game.phase == GAME_PHASE_DAY_DISCUSS);
    CHECK(game.phase_started_ms == 1500);
    CHECK(game.deadline_ms == 91500);
    CHECK(guard->last_guarded_id == villager_b->id);
    for (size_t i = 0; i < game.player_count; ++i)
    {
        CHECK(!game.players[i].night_action_submitted);
        CHECK(game.players[i].night_target_id == 0);
    }
    return true;
}

static bool test_invalid_actions_preserve_choices(void)
{
    struct game empty = {0};
    struct game_event_batch empty_events = {0};
    CHECK(night_action(&empty, 1, 2, 0, &empty_events) ==
          GAME_RESULT_INTERNAL);

    struct game game = {0};
    struct game_event_batch events = {0};
    CHECK(init_game(&game, 7, 200, 0, 22));
    struct game_player *wolf =
        player_with_role(&game, GAME_ROLE_WEREWOLF, 0);
    struct game_player *other_wolf =
        player_with_role(&game, GAME_ROLE_WEREWOLF, 1);
    struct game_player *seer = player_with_role(&game, GAME_ROLE_SEER, 0);
    struct game_player *guard = player_with_role(&game, GAME_ROLE_GUARD, 0);
    struct game_player *villager =
        player_with_role(&game, GAME_ROLE_VILLAGER, 0);
    struct game_player *other_villager =
        player_with_role(&game, GAME_ROLE_VILLAGER, 1);
    CHECK(wolf && other_wolf && seer && guard && villager && other_villager);

    struct game over = game;
    over.phase = GAME_PHASE_OVER;
    over.winner = GAME_FACTION_VILLAGE;
    CHECK(night_action(&over, wolf->id, villager->id, 1, &events) ==
          GAME_RESULT_WRONG_PHASE);
    CHECK(events.count == 0);

    CHECK(night_action(&game, wolf->id, villager->id, 10, &events) ==
          GAME_RESULT_OK);
    CHECK(night_action(&game, wolf->id, other_wolf->id, 20, &events) ==
          GAME_RESULT_INVALID_TARGET);
    CHECK(wolf->night_target_id == villager->id);
    CHECK(events.count == 0);

    CHECK(night_action(&game, seer->id, seer->id, 30, &events) ==
          GAME_RESULT_INVALID_TARGET);
    CHECK(!seer->night_action_submitted);
    CHECK(night_action(&game, seer->id, villager->id, 40, &events) ==
          GAME_RESULT_OK);
    CHECK(night_action(&game, seer->id, other_villager->id, 50, &events) ==
          GAME_RESULT_UNAUTHORIZED);
    CHECK(seer->night_target_id == villager->id);

    guard->last_guarded_id = villager->id;
    CHECK(night_action(&game, guard->id, villager->id, 60, &events) ==
          GAME_RESULT_INVALID_TARGET);
    CHECK(!guard->night_action_submitted);
    CHECK(night_action(&game, guard->id, guard->id, 70, &events) ==
          GAME_RESULT_OK);
    CHECK(night_action(&game, guard->id, other_villager->id, 80, &events) ==
          GAME_RESULT_UNAUTHORIZED);
    CHECK(guard->night_target_id == guard->id);

    CHECK(night_action(&game, villager->id, other_villager->id, 90,
                       &events) == GAME_RESULT_UNAUTHORIZED);
    CHECK(night_action(&game, 999999, villager->id, 100, &events) ==
          GAME_RESULT_UNAUTHORIZED);
    CHECK(night_action(&game, other_wolf->id, 999999, 110, &events) ==
          GAME_RESULT_INVALID_TARGET);

    other_wolf->alive = false;
    CHECK(night_action(&game, other_wolf->id, villager->id, 120, &events) ==
          GAME_RESULT_UNAUTHORIZED);
    other_wolf->alive = true;
    other_wolf->connected = false;
    CHECK(night_action(&game, other_wolf->id, villager->id, 130, &events) ==
          GAME_RESULT_UNAUTHORIZED);

    villager->alive = false;
    uint32_t saved_wolf_target = wolf->night_target_id;
    CHECK(night_action(&game, wolf->id, villager->id, 140, &events) ==
          GAME_RESULT_INVALID_TARGET);
    CHECK(wolf->night_target_id == saved_wolf_target);

    struct game wrong_phase = game;
    wrong_phase.phase = GAME_PHASE_DAY_DISCUSS;
    struct game saved = wrong_phase;
    CHECK(night_action(&wrong_phase, wolf->id, other_villager->id, 150,
                       &events) == GAME_RESULT_WRONG_PHASE);
    CHECK(events.count == 0);
    CHECK(memcmp(&wrong_phase, &saved, sizeof saved) == 0);
    return true;
}

static bool test_timeout_tie_and_no_actions(void)
{
    struct game no_actions = {0};
    struct game_event_batch events = {0};
    CHECK(init_game(&no_actions, 5, 300, 100, 33));
    struct game_player *guard =
        player_with_role(&no_actions, GAME_ROLE_GUARD, 0);
    CHECK(guard != NULL);
    guard->last_guarded_id = no_actions.players[0].id;

    struct game saved = no_actions;
    CHECK(game_handle_timeout(&no_actions, no_actions.deadline_ms - 1,
                              &events) == GAME_RESULT_OK);
    CHECK(events.count == 0);
    CHECK(memcmp(&no_actions, &saved, sizeof saved) == 0);

    CHECK(game_handle_timeout(&no_actions, no_actions.deadline_ms,
                              &events) == GAME_RESULT_OK);
    const struct game_event *result =
        find_event(&events, GAME_EVENT_NIGHT_RESULT, 0);
    CHECK(result != NULL);
    CHECK(result->data.night_result.killed_player_id == 0);
    CHECK(no_actions.phase == GAME_PHASE_DAY_DISCUSS);
    CHECK(guard->last_guarded_id == 0);

    /* Hai Sói chọn khác nhau thì hòa, không ai chết. */
    struct game tie = {0};
    CHECK(init_game(&tie, 7, 400, 0, 44));
    struct game_player *wolf_a =
        player_with_role(&tie, GAME_ROLE_WEREWOLF, 0);
    struct game_player *wolf_b =
        player_with_role(&tie, GAME_ROLE_WEREWOLF, 1);
    struct game_player *target_a =
        player_with_role(&tie, GAME_ROLE_VILLAGER, 0);
    struct game_player *target_b =
        player_with_role(&tie, GAME_ROLE_VILLAGER, 1);
    CHECK(wolf_a && wolf_b && target_a && target_b);
    CHECK(night_action(&tie, wolf_a->id, target_a->id, 1, &events) ==
          GAME_RESULT_OK);
    CHECK(night_action(&tie, wolf_b->id, target_b->id, 2, &events) ==
          GAME_RESULT_OK);
    CHECK(game_handle_timeout(&tie, tie.deadline_ms, &events) ==
          GAME_RESULT_OK);
    result = find_event(&events, GAME_EVENT_NIGHT_RESULT, 0);
    CHECK(result != NULL);
    CHECK(result->data.night_result.killed_player_id == 0);
    CHECK(target_a->alive && target_b->alive);
    CHECK(tie.phase == GAME_PHASE_DAY_DISCUSS);
    return true;
}

static bool test_majority_kill_waits_for_server_10(void)
{
    struct game game = {0};
    struct game_event_batch events = {0};
    CHECK(init_game(&game, 10, 500, 0, 55));
    struct game_player *wolf_a =
        player_with_role(&game, GAME_ROLE_WEREWOLF, 0);
    struct game_player *wolf_b =
        player_with_role(&game, GAME_ROLE_WEREWOLF, 1);
    struct game_player *wolf_c =
        player_with_role(&game, GAME_ROLE_WEREWOLF, 2);
    struct game_player *target =
        player_with_role(&game, GAME_ROLE_VILLAGER, 0);
    struct game_player *other_target =
        player_with_role(&game, GAME_ROLE_VILLAGER, 1);
    CHECK(wolf_a && wolf_b && wolf_c && target && other_target);

    CHECK(night_action(&game, wolf_a->id, target->id, 1, &events) ==
          GAME_RESULT_OK);
    CHECK(night_action(&game, wolf_b->id, target->id, 2, &events) ==
          GAME_RESULT_OK);
    CHECK(night_action(&game, wolf_c->id, other_target->id, 3, &events) ==
          GAME_RESULT_OK);
    CHECK(game_handle_timeout(&game, game.deadline_ms, &events) ==
          GAME_RESULT_OK);
    const struct game_event *result =
        find_event(&events, GAME_EVENT_NIGHT_RESULT, 0);
    CHECK(result != NULL);
    CHECK(result->data.night_result.killed_player_id == target->id);
    CHECK(!target->alive);
    CHECK(game.awaiting_win_check);
    CHECK(game.phase == GAME_PHASE_NIGHT);
    CHECK(game_next_deadline_ms(&game) == UINT64_MAX);
    CHECK(find_event(&events, GAME_EVENT_PHASE_CHANGE, 0) == NULL);

    struct game saved = game;
    CHECK(game_handle_timeout(&game, game.deadline_ms, &events) ==
          GAME_RESULT_OK);
    CHECK(events.count == 0);
    CHECK(memcmp(&game, &saved, sizeof saved) == 0);
    return true;
}

static bool test_late_command_and_independent_games(void)
{
    struct game late = {0};
    struct game_event_batch events = {0};
    CHECK(init_game(&late, 5, 600, 1000, 66));
    struct game_player *wolf =
        player_with_role(&late, GAME_ROLE_WEREWOLF, 0);
    struct game_player *target =
        player_with_role(&late, GAME_ROLE_VILLAGER, 0);
    CHECK(wolf && target);
    CHECK(night_action(&late, wolf->id, target->id, late.deadline_ms,
                       &events) == GAME_RESULT_WRONG_PHASE);
    CHECK(find_event(&events, GAME_EVENT_NIGHT_RESULT, 0) != NULL);
    CHECK(find_event(&events, GAME_EVENT_PHASE_CHANGE, 0) != NULL);
    CHECK(!wolf->night_action_submitted);
    CHECK(late.phase == GAME_PHASE_DAY_DISCUSS);

    struct game game_a = {0};
    struct game game_b = {0};
    CHECK(init_game(&game_a, 7, 700, 0, 77));
    CHECK(init_game(&game_b, 7, 800, 0, 88));
    struct game saved_b = game_b;
    struct game_player *wolf_a =
        player_with_role(&game_a, GAME_ROLE_WEREWOLF, 0);
    struct game_player *target_a =
        player_with_role(&game_a, GAME_ROLE_VILLAGER, 0);
    CHECK(wolf_a && target_a);
    CHECK(night_action(&game_a, wolf_a->id, target_a->id, 1, &events) ==
          GAME_RESULT_OK);
    CHECK(memcmp(&game_b, &saved_b, sizeof saved_b) == 0);
    return true;
}

int main(void)
{
    if (!test_valid_actions_and_private_events() ||
        !test_invalid_actions_preserve_choices() ||
        !test_timeout_tie_and_no_actions() ||
        !test_majority_kill_waits_for_server_10() ||
        !test_late_command_and_independent_games())
    {
        return 1;
    }
    printf("PASS: game night tests\n");
    return 0;
}
