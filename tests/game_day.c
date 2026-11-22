#include "server/game.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                     \
    do                                                                       \
    {                                                                        \
        if (!(condition))                                                    \
        {                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,        \
                    #condition);                                             \
            return false;                                                    \
        }                                                                    \
    } while (0)

static bool init_game(struct game *game, size_t count, uint32_t base,
                      uint64_t now, uint64_t seed)
{
    uint32_t ids[PROTOCOL_MAX_PLAYERS];
    for (size_t i = 0; i < count; ++i)
        ids[i] = base + (uint32_t)i;
    const struct game_start_input input = {
        .player_ids = ids, .player_count = count,
        .now_ms = now, .random_seed = seed
    };
    struct game_event_batch events = {0};
    return game_init(game, &input, &events) == GAME_RESULT_OK;
}

static struct game_player *role_player(struct game *game,
                                       enum game_role role, size_t occurrence)
{
    for (size_t i = 0; i < game->player_count; ++i)
        if (game->players[i].role == role && occurrence-- == 0)
            return &game->players[i];
    return NULL;
}

static bool has_recipient(const struct game_event *event, uint32_t id)
{
    for (size_t i = 0; i < event->recipient_count; ++i)
        if (event->recipient_ids[i] == id)
            return true;
    return false;
}

static const struct game_event *event_of(const struct game_event_batch *events,
                                         enum game_event_type type)
{
    for (size_t i = 0; i < events->count; ++i)
        if (events->events[i].type == type)
            return &events->events[i];
    return NULL;
}

static enum game_result chat(struct game *game, uint32_t actor,
                             enum game_chat_scope scope, const char *text,
                             struct game_event_batch *events)
{
    const struct game_command command = {
        .type = GAME_COMMAND_CHAT,
        .data.chat = { .scope = scope, .message = (const uint8_t *)text,
                       .message_length = strlen(text) }
    };
    return game_handle_command(game, actor, 1, &command, events);
}

static enum game_result vote(struct game *game, uint32_t actor,
                             uint32_t target,
                             struct game_event_batch *events)
{
    const struct game_command command = {
        .type = GAME_COMMAND_DAY_VOTE,
        .data.day_vote.target_player_id = target
    };
    return game_handle_command(game, actor, 1, &command, events);
}

static void set_phase(struct game *game, enum game_phase phase)
{
    game->phase = phase;
    game->phase_started_ms = 100;
    uint64_t duration = phase == GAME_PHASE_DAY_DISCUSS
        ? GAME_DAY_DISCUSS_DURATION_SEC : GAME_DAY_VOTE_DURATION_SEC;
    game->deadline_ms = 100 + duration * UINT64_C(1000);
}

static bool test_day_and_wolf_chat_rules(void)
{
    struct game game = {0};
    struct game_event_batch events = {0};
    CHECK(init_game(&game, 7, 100, 0, 11));
    struct game_player *wolf = role_player(&game, GAME_ROLE_WEREWOLF, 0);
    struct game_player *dead_wolf = role_player(&game, GAME_ROLE_WEREWOLF, 1);
    struct game_player *villager = role_player(&game, GAME_ROLE_VILLAGER, 0);
    CHECK(wolf && dead_wolf && villager);

    set_phase(&game, GAME_PHASE_DAY_DISCUSS);
    dead_wolf->alive = false;
    CHECK(chat(&game, villager->id, GAME_CHAT_VILLAGE, "Xin chao", &events) == GAME_RESULT_OK);
    const struct game_event *message = event_of(&events, GAME_EVENT_CHAT);
    CHECK(message && message->recipient_count == game.player_count);
    CHECK(message->data.chat.scope == GAME_CHAT_VILLAGE);
    CHECK(has_recipient(message, villager->id));
    CHECK(has_recipient(message, dead_wolf->id));
    events.count = 0;
    CHECK(chat(&game, wolf->id, GAME_CHAT_WEREWOLF, "ban dem", &events) == GAME_RESULT_WRONG_PHASE);
    CHECK(events.count == 0);

    set_phase(&game, GAME_PHASE_NIGHT);
    dead_wolf->alive = false;
    CHECK(chat(&game, wolf->id, GAME_CHAT_WEREWOLF, "chi soi song", &events) == GAME_RESULT_OK);
    message = event_of(&events, GAME_EVENT_CHAT);
    CHECK(message && message->recipient_count == 1);
    CHECK(has_recipient(message, wolf->id));
    CHECK(!has_recipient(message, dead_wolf->id));
    CHECK(!has_recipient(message, villager->id));
    events.count = 0;
    CHECK(chat(&game, villager->id, GAME_CHAT_WEREWOLF, "khong phai soi", &events) == GAME_RESULT_UNAUTHORIZED);
    CHECK(chat(&game, villager->id, GAME_CHAT_VILLAGE, "sai pha", &events) == GAME_RESULT_WRONG_PHASE);
    CHECK(events.count == 0);
    CHECK(chat(&game, dead_wolf->id, GAME_CHAT_WEREWOLF, "da chet", &events) == GAME_RESULT_UNAUTHORIZED);
    return true;
}

static bool test_vote_validation_and_winner(void)
{
    struct game game = {0};
    struct game_event_batch events = {0};
    CHECK(init_game(&game, 5, 200, 0, 22));
    struct game_player *wolf = role_player(&game, GAME_ROLE_WEREWOLF, 0);
    CHECK(wolf != NULL);
    set_phase(&game, GAME_PHASE_DAY_VOTE);
    CHECK(vote(&game, wolf->id, wolf->id, &events) == GAME_RESULT_INVALID_TARGET);
    CHECK(!wolf->vote_submitted);

    struct game_player *voters[PROTOCOL_MAX_PLAYERS];
    size_t count = 0;
    for (size_t i = 0; i < game.player_count; ++i)
        if (game.players[i].id != wolf->id)
            voters[count++] = &game.players[i];
    for (size_t i = 0; i < count; ++i)
        CHECK(vote(&game, voters[i]->id, wolf->id, &events) == GAME_RESULT_OK);
    CHECK(vote(&game, wolf->id, voters[0]->id, &events) == GAME_RESULT_OK);
    CHECK(game_is_over(&game));
    CHECK(game.winner == GAME_FACTION_VILLAGE);
    CHECK(!wolf->alive);
    CHECK(event_of(&events, GAME_EVENT_DAY_RESULT)->data.day_result.executed_player_id == wolf->id);
    CHECK(event_of(&events, GAME_EVENT_GAME_OVER)->data.game_over.winner == GAME_FACTION_VILLAGE);
    return true;
}

static bool test_repeat_vote_dead_voter_and_tie_white(void)
{
    struct game game = {0};
    struct game_event_batch events = {0};
    CHECK(init_game(&game, 7, 300, 0, 33));
    set_phase(&game, GAME_PHASE_DAY_VOTE);
    struct game_player *voter = &game.players[0];
    struct game_player *target_a = &game.players[1];
    struct game_player *target_b = &game.players[2];
    CHECK(vote(&game, voter->id, target_a->id, &events) == GAME_RESULT_OK);
    events.count = 0;
    CHECK(vote(&game, voter->id, target_b->id, &events) == GAME_RESULT_UNAUTHORIZED);
    CHECK(voter->vote_target_id == target_a->id);
    CHECK(events.count == 0);
    struct game_player *dead = &game.players[3];
    dead->alive = false;
    CHECK(vote(&game, dead->id, 0, &events) == GAME_RESULT_UNAUTHORIZED);

    /* Complete ballots with a tie between two living targets. */
    bool target_a_voted = true, target_b_voted = false;
    for (size_t i = 0; i < game.player_count; ++i)
    {
        struct game_player *p = &game.players[i];
        if (!p->alive || p->vote_submitted) continue;
        uint32_t choice = 0;
        if (p->id != target_a->id && !target_a_voted) {
            choice = target_a->id; target_a_voted = true;
        } else if (p->id != target_b->id && !target_b_voted) {
            choice = target_b->id; target_b_voted = true;
        }
        CHECK(vote(&game, p->id, choice, &events) == GAME_RESULT_OK);
    }
    const struct game_event *result = event_of(&events, GAME_EVENT_DAY_RESULT);
    CHECK(result && result->data.day_result.executed_player_id == 0);
    CHECK(game.phase == GAME_PHASE_NIGHT);
    CHECK(target_a->alive && target_b->alive);

    struct game white = {0};
    CHECK(init_game(&white, 5, 400, 0, 44));
    set_phase(&white, GAME_PHASE_DAY_VOTE);
    memset(&events, 0, sizeof events);
    for (size_t i = 0; i < white.player_count; ++i)
        CHECK(vote(&white, white.players[i].id, 0, &events) == GAME_RESULT_OK);
    result = event_of(&events, GAME_EVENT_DAY_RESULT);
    CHECK(result && result->data.day_result.executed_player_id == 0);
    CHECK(white.phase == GAME_PHASE_NIGHT);
    return true;
}

static bool test_timeout_closes_voting_and_discussion(void)
{
    struct game game = {0};
    struct game_event_batch events = {0};
    CHECK(init_game(&game, 5, 500, 0, 55));
    set_phase(&game, GAME_PHASE_DAY_DISCUSS);
    CHECK(game_handle_timeout(&game, game.deadline_ms, &events) == GAME_RESULT_OK);
    CHECK(game.phase == GAME_PHASE_DAY_VOTE);
    CHECK(game.deadline_ms == UINT64_C(120100));
    events.count = 0;
    CHECK(game_handle_timeout(&game, game.deadline_ms, &events) == GAME_RESULT_OK);
    const struct game_event *result = event_of(&events, GAME_EVENT_DAY_RESULT);
    CHECK(result && result->data.day_result.executed_player_id == 0);
    CHECK(game.phase == GAME_PHASE_NIGHT);
    CHECK(game.round_number == 2);
    return true;
}

int main(void)
{
    if (!test_day_and_wolf_chat_rules() || !test_vote_validation_and_winner() ||
        !test_repeat_vote_dead_voter_and_tie_white() ||
        !test_timeout_closes_voting_and_discussion()) return 1;
    puts("PASS: game day chat and voting tests");
    return 0;
}
