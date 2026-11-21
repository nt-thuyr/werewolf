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

static uint8_t expected_wolves(size_t player_count)
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

static bool contains_id(const uint32_t *ids, size_t count, uint32_t id)
{
    for (size_t i = 0; i < count; ++i)
    {
        if (ids[i] == id)
        {
            return true;
        }
    }
    return false;
}

static bool check_role_counts(const struct game *game)
{
    size_t wolves = 0;
    size_t villagers = 0;
    size_t seers = 0;
    size_t guards = 0;

    for (size_t i = 0; i < game->player_count; ++i)
    {
        switch (game->players[i].role)
        {
        case GAME_ROLE_WEREWOLF:
            ++wolves;
            break;
        case GAME_ROLE_VILLAGER:
            ++villagers;
            break;
        case GAME_ROLE_SEER:
            ++seers;
            break;
        case GAME_ROLE_GUARD:
            ++guards;
            break;
        default:
            return false;
        }
    }

    return wolves == expected_wolves(game->player_count) && seers == 1 &&
           guards == 1 &&
           villagers == game->player_count - wolves - seers - guards;
}

static bool check_initial_events(const struct game *game,
                                 const struct game_event_batch *events)
{
    CHECK(events->count == (size_t)game->player_count + 2);
    CHECK(events->count <= GAME_MAX_EVENTS);

    for (size_t i = 0; i < game->player_count; ++i)
    {
        const struct game_event *event = &events->events[i];
        CHECK(event->type == GAME_EVENT_ROLE_ASSIGN);
        CHECK(event->recipient_count == 1);
        CHECK(event->recipient_ids[0] == game->players[i].id);
        CHECK(event->data.role_assign.role == game->players[i].role);
    }

    const struct game_event *wolf_event =
        &events->events[game->player_count];
    CHECK(wolf_event->type == GAME_EVENT_WOLF_TEAM_INFO);
    CHECK(wolf_event->recipient_count == expected_wolves(game->player_count));
    CHECK(wolf_event->data.wolf_team_info.wolf_count ==
          wolf_event->recipient_count);
    for (size_t i = 0; i < wolf_event->recipient_count; ++i)
    {
        uint32_t wolf_id = wolf_event->recipient_ids[i];
        const struct game_player *wolf = game_find_player(game, wolf_id);
        CHECK(wolf != NULL);
        CHECK(wolf->role == GAME_ROLE_WEREWOLF);
        CHECK(contains_id(wolf_event->data.wolf_team_info.wolf_ids,
                          wolf_event->data.wolf_team_info.wolf_count,
                          wolf_id));
    }
    for (size_t i = 0; i < game->player_count; ++i)
    {
        bool is_wolf = game->players[i].role == GAME_ROLE_WEREWOLF;
        CHECK(contains_id(wolf_event->recipient_ids,
                          wolf_event->recipient_count,
                          game->players[i].id) == is_wolf);
        CHECK(contains_id(wolf_event->data.wolf_team_info.wolf_ids,
                          wolf_event->data.wolf_team_info.wolf_count,
                          game->players[i].id) == is_wolf);
    }

    const struct game_event *phase_event =
        &events->events[game->player_count + 1];
    CHECK(phase_event->type == GAME_EVENT_PHASE_CHANGE);
    CHECK(phase_event->recipient_count == game->player_count);
    CHECK(phase_event->data.phase_change.phase == GAME_PHASE_NIGHT);
    CHECK(phase_event->data.phase_change.duration_sec ==
          GAME_NIGHT_DURATION_SEC);
    CHECK(phase_event->data.phase_change.deadline_ms == game->deadline_ms);
    for (size_t i = 0; i < game->player_count; ++i)
    {
        CHECK(phase_event->recipient_ids[i] == game->players[i].id);
    }
    return true;
}

static bool test_valid_initialization(void)
{
    uint32_t ids[] = {101, 102, 103, 104, 105};
    struct game game = {0};
    struct game_event_batch events = {0};
    const struct game_start_input input = {
        .player_ids = ids,
        .player_count = 5,
        .now_ms = UINT64_C(123456),
        .random_seed = UINT64_C(987654321)
    };

    CHECK(game_init(&game, &input, &events) == GAME_RESULT_OK);
    CHECK(game.initialized);
    CHECK(game.player_count == 5);
    CHECK(game.phase == GAME_PHASE_NIGHT);
    CHECK(game.winner == GAME_FACTION_NONE);
    CHECK(game.round_number == 1);
    CHECK(game.phase_started_ms == input.now_ms);
    CHECK(game.deadline_ms == input.now_ms + UINT64_C(30000));
    CHECK(game_next_deadline_ms(&game) == game.deadline_ms);
    CHECK(!game_is_over(&game));
    CHECK(game_find_player(&game, 999) == NULL);
    CHECK(game_find_player(NULL, 101) == NULL);

    for (size_t i = 0; i < game.player_count; ++i)
    {
        const struct game_player *player = &game.players[i];
        CHECK(player->id == ids[i]);
        CHECK(player->alive);
        CHECK(player->connected);
        CHECK(!player->night_action_submitted);
        CHECK(player->night_target_id == 0);
        CHECK(!player->vote_submitted);
        CHECK(player->vote_target_id == 0);
        CHECK(player->last_guarded_id == 0);
    }
    CHECK(check_role_counts(&game));
    CHECK(check_initial_events(&game, &events));

    /* Chứng minh game đã sao chép ID, không giữ mảng của bên gọi. */
    ids[0] = 9999;
    CHECK(game.players[0].id == 101);
    CHECK(events.events[0].recipient_ids[0] == 101);
    CHECK(events.events[game.player_count + 1].recipient_ids[0] == 101);
    return true;
}

static bool test_role_composition_boundaries(void)
{
    const size_t counts[] = {5, 6, 7, 8, 9, 10, 11, 12};
    for (size_t case_index = 0;
         case_index < sizeof counts / sizeof counts[0]; ++case_index)
    {
        uint32_t ids[PROTOCOL_MAX_PLAYERS];
        for (size_t i = 0; i < counts[case_index]; ++i)
        {
            ids[i] = (uint32_t)(1000 + case_index * 100 + i);
        }
        struct game game = {0};
        struct game_event_batch events = {0};
        const struct game_start_input input = {
            .player_ids = ids,
            .player_count = counts[case_index],
            .now_ms = 0,
            .random_seed = (uint64_t)case_index + 1
        };
        CHECK(game_init(&game, &input, &events) == GAME_RESULT_OK);
        CHECK(check_role_counts(&game));
        CHECK(check_initial_events(&game, &events));
    }
    return true;
}

static bool test_reinitialize_clears_old_game(void)
{
    uint32_t old_ids[] = {101, 102, 103, 104, 105, 106,
                          107, 108, 109, 110, 111, 112};
    uint32_t new_ids[] = {201, 202, 203, 204, 205};
    struct game game = {0};
    struct game_event_batch events = {0};
    struct game_start_input input = {
        .player_ids = old_ids,
        .player_count = 12,
        .now_ms = 1000,
        .random_seed = 11
    };

    CHECK(game_init(&game, &input, &events) == GAME_RESULT_OK);
    game.players[0].alive = false;
    game.players[1].night_action_submitted = true;
    game.players[1].night_target_id = old_ids[2];
    game.players[2].vote_submitted = true;
    game.players[2].vote_target_id = old_ids[3];
    game.players[3].last_guarded_id = old_ids[4];

    input.player_ids = new_ids;
    input.player_count = 5;
    input.now_ms = 9000;
    input.random_seed = 22;
    CHECK(game_init(&game, &input, &events) == GAME_RESULT_OK);
    CHECK(game.player_count == 5);
    CHECK(game.phase == GAME_PHASE_NIGHT);
    CHECK(game.phase_started_ms == 9000);
    CHECK(game.deadline_ms == 39000);
    CHECK(check_role_counts(&game));
    CHECK(check_initial_events(&game, &events));

    for (size_t i = 0; i < game.player_count; ++i)
    {
        CHECK(game.players[i].id == new_ids[i]);
        CHECK(game.players[i].alive);
        CHECK(game.players[i].connected);
        CHECK(!game.players[i].night_action_submitted);
        CHECK(game.players[i].night_target_id == 0);
        CHECK(!game.players[i].vote_submitted);
        CHECK(game.players[i].vote_target_id == 0);
        CHECK(game.players[i].last_guarded_id == 0);
    }
    for (size_t i = game.player_count; i < PROTOCOL_MAX_PLAYERS; ++i)
    {
        const struct game_player empty = {0};
        CHECK(memcmp(&game.players[i], &empty, sizeof empty) == 0);
    }
    for (size_t i = 0; i < sizeof old_ids / sizeof old_ids[0]; ++i)
    {
        CHECK(game_find_player(&game, old_ids[i]) == NULL);
    }
    return true;
}

static bool test_invalid_inputs_are_atomic(void)
{
    uint32_t valid_ids[] = {1, 2, 3, 4, 5};
    struct game game = {0};
    struct game_event_batch events = {0};
    struct game_start_input input = {
        .player_ids = valid_ids,
        .player_count = 5,
        .now_ms = 100,
        .random_seed = 123
    };
    CHECK(game_init(&game, &input, &events) == GAME_RESULT_OK);
    struct game saved = game;

    uint32_t duplicate_ids[] = {1, 2, 2, 4, 5};
    input.player_ids = duplicate_ids;
    events.count = 99;
    CHECK(game_init(&game, &input, &events) == GAME_RESULT_INVALID_PAYLOAD);
    CHECK(events.count == 0);
    CHECK(memcmp(&game, &saved, sizeof game) == 0);

    uint32_t zero_ids[] = {1, 2, 0, 4, 5};
    input.player_ids = zero_ids;
    CHECK(game_init(&game, &input, &events) == GAME_RESULT_INVALID_PAYLOAD);
    CHECK(memcmp(&game, &saved, sizeof game) == 0);

    input.player_ids = valid_ids;
    input.player_count = 4;
    CHECK(game_init(&game, &input, &events) ==
          GAME_RESULT_NOT_ENOUGH_PLAYERS);
    CHECK(memcmp(&game, &saved, sizeof game) == 0);

    uint32_t too_many_ids[PROTOCOL_MAX_PLAYERS + 1];
    for (size_t i = 0; i < PROTOCOL_MAX_PLAYERS + 1; ++i)
    {
        too_many_ids[i] = (uint32_t)i + 1;
    }
    input.player_ids = too_many_ids;
    input.player_count = PROTOCOL_MAX_PLAYERS + 1;
    CHECK(game_init(&game, &input, &events) == GAME_RESULT_INVALID_PAYLOAD);
    CHECK(memcmp(&game, &saved, sizeof game) == 0);

    input.player_ids = valid_ids;
    input.player_count = 5;
    input.random_seed = 0;
    CHECK(game_init(&game, &input, &events) == GAME_RESULT_INVALID_PAYLOAD);
    CHECK(memcmp(&game, &saved, sizeof game) == 0);

    input.random_seed = 1;
    input.now_ms = UINT64_MAX - UINT64_C(29999);
    CHECK(game_init(&game, &input, &events) == GAME_RESULT_INVALID_PAYLOAD);
    CHECK(memcmp(&game, &saved, sizeof game) == 0);

    input.now_ms = 0;
    input.player_ids = NULL;
    CHECK(game_init(&game, &input, &events) == GAME_RESULT_INVALID_PAYLOAD);
    CHECK(game_init(&game, NULL, &events) == GAME_RESULT_INVALID_PAYLOAD);
    CHECK(game_init(NULL, &input, &events) == GAME_RESULT_INTERNAL);
    CHECK(game_init(&game, &input, NULL) == GAME_RESULT_INTERNAL);
    CHECK(memcmp(&game, &saved, sizeof game) == 0);
    return true;
}

static bool test_repeatable_seed_and_independent_games(void)
{
    uint32_t ids_a[] = {11, 12, 13, 14, 15, 16, 17, 18};
    uint32_t ids_b[] = {21, 22, 23, 24, 25, 26, 27, 28};
    struct game game_a = {0};
    struct game same_seed = {0};
    struct game game_b = {0};
    struct game_event_batch events = {0};
    const struct game_start_input input_a = {
        .player_ids = ids_a,
        .player_count = 8,
        .now_ms = 5000,
        .random_seed = UINT64_C(555)
    };
    struct game_start_input input_b = input_a;

    CHECK(game_init(&game_a, &input_a, &events) == GAME_RESULT_OK);
    CHECK(game_init(&same_seed, &input_a, &events) == GAME_RESULT_OK);
    for (size_t i = 0; i < input_a.player_count; ++i)
    {
        CHECK(game_a.players[i].role == same_seed.players[i].role);
    }

    struct game saved_a = game_a;
    input_b.player_ids = ids_b;
    input_b.random_seed = 777;
    CHECK(game_init(&game_b, &input_b, &events) == GAME_RESULT_OK);
    CHECK(memcmp(&game_a, &saved_a, sizeof game_a) == 0);
    CHECK(game_find_player(&game_a, ids_b[0]) == NULL);
    CHECK(game_find_player(&game_b, ids_b[0]) != NULL);
    return true;
}

static bool test_helpers_and_unimplemented_handlers(void)
{
    struct game empty = {0};
    CHECK(game_next_deadline_ms(NULL) == UINT64_MAX);
    CHECK(game_next_deadline_ms(&empty) == UINT64_MAX);
    CHECK(!game_is_over(NULL));
    CHECK(!game_is_over(&empty));

    empty.initialized = true;
    empty.phase = GAME_PHASE_OVER;
    CHECK(game_is_over(&empty));
    CHECK(game_next_deadline_ms(&empty) == UINT64_MAX);

    struct game_event_batch events = {.count = 10};
    struct game_command command = {.type = GAME_COMMAND_NONE};
    struct game saved = empty;
    CHECK(game_handle_command(&empty, 1, 0, &command, &events) ==
          GAME_RESULT_INTERNAL);
    CHECK(events.count == 0);
    CHECK(memcmp(&empty, &saved, sizeof empty) == 0);
    events.count = 10;
    CHECK(game_handle_timeout(&empty, 0, &events) == GAME_RESULT_INTERNAL);
    CHECK(events.count == 0);
    CHECK(memcmp(&empty, &saved, sizeof empty) == 0);
    events.count = 10;
    CHECK(game_handle_player_left(&empty, 1, 0, &events) ==
          GAME_RESULT_INTERNAL);
    CHECK(events.count == 0);
    CHECK(memcmp(&empty, &saved, sizeof empty) == 0);
    return true;
}

int main(void)
{
    if (!test_valid_initialization() ||
        !test_role_composition_boundaries() ||
        !test_reinitialize_clears_old_game() ||
        !test_invalid_inputs_are_atomic() ||
        !test_repeatable_seed_and_independent_games() ||
        !test_helpers_and_unimplemented_handlers())
    {
        return 1;
    }
    printf("PASS: game initialization tests\n");
    return 0;
}
