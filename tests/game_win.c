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

static bool init_game(struct game *game, uint32_t base_id, uint64_t seed)
{
    uint32_t ids[] = {base_id, base_id + 1, base_id + 2,
                      base_id + 3, base_id + 4};
    const struct game_start_input input = {
        .player_ids = ids,
        .player_count = 5,
        .now_ms = 1000,
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

static bool check_game_over_event(const struct game *game,
                                  const struct game_event *event,
                                  enum game_faction winner)
{
    CHECK(event->type == GAME_EVENT_GAME_OVER);
    CHECK(event->data.game_over.winner == winner);
    CHECK(event->data.game_over.summary_length == 0);
    for (size_t i = 0; i < game->player_count; ++i)
    {
        CHECK(has_recipient(event, game->players[i].id) ==
              game->players[i].connected);
    }
    return true;
}

static bool test_no_winner_does_not_change_game(void)
{
    struct game game = {0};
    struct game_event_batch events = {0};
    CHECK(init_game(&game, 100, 11));
    struct game saved = game;

    CHECK(game_check_winner(&game, &events) == GAME_RESULT_OK);
    CHECK(events.count == 0);
    CHECK(memcmp(&game, &saved, sizeof saved) == 0);
    return true;
}

static bool test_village_win_and_event_order(void)
{
    struct game game = {0};
    struct game_event_batch events = {0};
    CHECK(init_game(&game, 200, 22));
    struct game_player *wolf =
        player_with_role(&game, GAME_ROLE_WEREWOLF, 0);
    CHECK(wolf != NULL);
    wolf->alive = false;

    /* Mô phỏng kết quả pha đã được xếp trước khi xét thắng. */
    events.count = 1;
    events.events[0] = (struct game_event){
        .type = GAME_EVENT_DAY_RESULT
    };
    CHECK(game_check_winner(&game, &events) == GAME_RESULT_OK);
    CHECK(events.count == 2);
    CHECK(events.events[0].type == GAME_EVENT_DAY_RESULT);
    CHECK(check_game_over_event(&game, &events.events[1],
                                GAME_FACTION_VILLAGE));
    CHECK(game_is_over(&game));
    CHECK(game.winner == GAME_FACTION_VILLAGE);
    CHECK(game.deadline_ms == UINT64_MAX);
    CHECK(game_next_deadline_ms(&game) == UINT64_MAX);

    /* Gọi lại không phát GAME_OVER lần hai. */
    CHECK(game_check_winner(&game, &events) == GAME_RESULT_OK);
    CHECK(events.count == 2);
    return true;
}

static bool test_werewolf_win(void)
{
    struct game game = {0};
    struct game_event_batch events = {0};
    CHECK(init_game(&game, 300, 33));
    struct game_player *wolf =
        player_with_role(&game, GAME_ROLE_WEREWOLF, 0);
    struct game_player *survivor =
        player_with_role(&game, GAME_ROLE_VILLAGER, 0);
    CHECK(wolf && survivor);

    for (size_t i = 0; i < game.player_count; ++i)
    {
        if (game.players[i].id != wolf->id &&
            game.players[i].id != survivor->id)
        {
            game.players[i].alive = false;
        }
    }

    CHECK(game_check_winner(&game, &events) == GAME_RESULT_OK);
    CHECK(events.count == 1);
    CHECK(check_game_over_event(&game, &events.events[0],
                                GAME_FACTION_WEREWOLF));
    CHECK(game_is_over(&game));
    CHECK(game.winner == GAME_FACTION_WEREWOLF);
    return true;
}

static bool test_failure_is_atomic(void)
{
    struct game game = {0};
    struct game_event_batch events = {.count = GAME_MAX_EVENTS};
    CHECK(init_game(&game, 400, 44));
    struct game_player *wolf =
        player_with_role(&game, GAME_ROLE_WEREWOLF, 0);
    CHECK(wolf != NULL);
    wolf->alive = false;
    struct game saved = game;

    CHECK(game_check_winner(&game, &events) == GAME_RESULT_INTERNAL);
    CHECK(events.count == GAME_MAX_EVENTS);
    CHECK(memcmp(&game, &saved, sizeof saved) == 0);

    struct game empty = {0};
    CHECK(game_check_winner(&empty, &events) == GAME_RESULT_INTERNAL);
    CHECK(game_check_winner(NULL, &events) == GAME_RESULT_INTERNAL);
    CHECK(game_check_winner(&game, NULL) == GAME_RESULT_INTERNAL);
    return true;
}

static bool test_night_result_then_werewolf_win(void)
{
    struct game game = {0};
    struct game_event_batch events = {0};
    CHECK(init_game(&game, 500, 55));
    struct game_player *wolf =
        player_with_role(&game, GAME_ROLE_WEREWOLF, 0);
    struct game_player *seer = player_with_role(&game, GAME_ROLE_SEER, 0);
    struct game_player *guard = player_with_role(&game, GAME_ROLE_GUARD, 0);
    struct game_player *target =
        player_with_role(&game, GAME_ROLE_VILLAGER, 0);
    CHECK(wolf && seer && guard && target);

    seer->alive = false;
    guard->alive = false;
    const struct game_command command = {
        .type = GAME_COMMAND_NIGHT_ACTION,
        .data.night_action.target_player_id = target->id
    };
    CHECK(game_handle_command(&game, wolf->id, 1100, &command, &events) ==
          GAME_RESULT_OK);
    CHECK(events.count == 2);
    CHECK(events.events[0].type == GAME_EVENT_NIGHT_RESULT);
    CHECK(events.events[0].data.night_result.killed_player_id == target->id);
    CHECK(events.events[1].type == GAME_EVENT_GAME_OVER);
    CHECK(events.events[1].data.game_over.winner ==
          GAME_FACTION_WEREWOLF);
    CHECK(game_is_over(&game));
    return true;
}

static bool test_two_games_and_new_game_reset(void)
{
    struct game game_a = {0};
    struct game game_b = {0};
    struct game_event_batch events = {0};
    CHECK(init_game(&game_a, 600, 66));
    CHECK(init_game(&game_b, 700, 77));
    struct game saved_b = game_b;
    struct game_player *wolf =
        player_with_role(&game_a, GAME_ROLE_WEREWOLF, 0);
    CHECK(wolf != NULL);
    wolf->alive = false;
    CHECK(game_check_winner(&game_a, &events) == GAME_RESULT_OK);
    CHECK(memcmp(&game_b, &saved_b, sizeof saved_b) == 0);

    uint32_t new_ids[] = {801, 802, 803, 804, 805};
    const struct game_start_input input = {
        .player_ids = new_ids,
        .player_count = 5,
        .now_ms = 9000,
        .random_seed = 88
    };
    CHECK(game_init(&game_a, &input, &events) == GAME_RESULT_OK);
    CHECK(!game_is_over(&game_a));
    CHECK(game_a.phase == GAME_PHASE_NIGHT);
    CHECK(game_a.winner == GAME_FACTION_NONE);
    return true;
}

int main(void)
{
    if (!test_no_winner_does_not_change_game() ||
        !test_village_win_and_event_order() ||
        !test_werewolf_win() ||
        !test_failure_is_atomic() ||
        !test_night_result_then_werewolf_win() ||
        !test_two_games_and_new_game_reset())
    {
        return 1;
    }
    printf("PASS: game winner tests\n");
    return 0;
}
