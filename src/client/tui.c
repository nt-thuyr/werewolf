#define _XOPEN_SOURCE_EXTENDED 1

#include "client/tui.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <locale.h>
#include <ncursesw/curses.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wctype.h>

enum {
    TUI_MIN_ROWS = 20,
    TUI_MIN_COLS = 70,
    TUI_LOG_CAPACITY = 128,
    TUI_LOG_BYTES = 640
};

enum {
    PAIR_HEADER = 1,
    PAIR_SYSTEM,
    PAIR_SUCCESS,
    PAIR_ERROR,
    PAIR_VILLAGE,
    PAIR_WOLVES,
    PAIR_NIGHT,
    PAIR_VOTE,
    PAIR_DIM
};

struct tui_log_entry {
    enum tui_log_kind kind;
    char text[TUI_LOG_BYTES];
};

static SCREEN *screen;
static WINDOW *header_window;
static WINDOW *players_window;
static WINDOW *info_window;
static WINDOW *log_window;
static WINDOW *input_window;
static struct tui_log_entry logs[TUI_LOG_CAPACITY];
static size_t log_start;
static size_t log_count;
static int saved_stdout = -1;
static int layout_rows;
static int layout_columns;

static int pair_for_log(enum tui_log_kind kind)
{
    switch (kind) {
    case TUI_LOG_SUCCESS: return PAIR_SUCCESS;
    case TUI_LOG_ERROR: return PAIR_ERROR;
    case TUI_LOG_VILLAGE: return PAIR_VILLAGE;
    case TUI_LOG_WOLVES: return PAIR_WOLVES;
    case TUI_LOG_NIGHT: return PAIR_NIGHT;
    case TUI_LOG_VOTE: return PAIR_VOTE;
    case TUI_LOG_SYSTEM: return PAIR_SYSTEM;
    }
    return PAIR_SYSTEM;
}

static const char *label_for_log(enum tui_log_kind kind)
{
    switch (kind) {
    case TUI_LOG_SUCCESS: return "OK";
    case TUI_LOG_ERROR: return "ERROR";
    case TUI_LOG_VILLAGE: return "VILLAGE";
    case TUI_LOG_WOLVES: return "WOLVES";
    case TUI_LOG_NIGHT: return "NIGHT";
    case TUI_LOG_VOTE: return "VOTE";
    case TUI_LOG_SYSTEM: return "SYSTEM";
    }
    return "SYSTEM";
}

static const char *role_name(enum protocol_role role)
{
    switch (role) {
    case PROTOCOL_ROLE_WEREWOLF: return "Werewolf";
    case PROTOCOL_ROLE_VILLAGER: return "Villager";
    case PROTOCOL_ROLE_SEER: return "Seer";
    case PROTOCOL_ROLE_GUARD: return "Guard";
    }
    return "Unknown";
}

static const char *phase_name(enum protocol_phase phase)
{
    switch (phase) {
    case PROTOCOL_PHASE_NIGHT: return "NIGHT";
    case PROTOCOL_PHASE_DAY_DISCUSS: return "DAY CHAT";
    case PROTOCOL_PHASE_DAY_VOTE: return "DAY VOTE";
    }
    return "WAITING";
}

static void destroy_windows(void)
{
    if (header_window != NULL) delwin(header_window);
    if (players_window != NULL) delwin(players_window);
    if (info_window != NULL) delwin(info_window);
    if (log_window != NULL) delwin(log_window);
    if (input_window != NULL) delwin(input_window);
    header_window = players_window = info_window = log_window = input_window = NULL;
}

static bool create_windows(void)
{
    destroy_windows();
    int rows;
    int columns;
    getmaxyx(stdscr, rows, columns);
    if (rows < TUI_MIN_ROWS || columns < TUI_MIN_COLS) {
        return false;
    }

    int header_height = 4;
    int input_height = 3;
    int summary_height = rows >= 28 ? 11 : 9;
    int content_width = columns / 2;
    int log_height = rows - header_height - summary_height - input_height;
    header_window = newwin(header_height, columns, 0, 0);
    players_window = newwin(summary_height, content_width, header_height, 0);
    info_window = newwin(summary_height, columns - content_width,
                         header_height, content_width);
    log_window = newwin(log_height, columns, header_height + summary_height, 0);
    input_window = newwin(input_height, columns, rows - input_height, 0);
    layout_rows = rows;
    layout_columns = columns;
    return header_window != NULL && players_window != NULL &&
           info_window != NULL && log_window != NULL && input_window != NULL;
}

static void title(WINDOW *window, const char *text, int pair)
{
    wattron(window, COLOR_PAIR(pair) | A_BOLD);
    mvwprintw(window, 0, 2, " %s ", text);
    wattroff(window, COLOR_PAIR(pair) | A_BOLD);
}

static void safe_add(WINDOW *window, const char *text, int limit)
{
    if (text == NULL) return;
    char clean[1024];
    size_t used = 0;
    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor != '\0' && used < sizeof clean - 1 &&
         used < (size_t)limit; ++cursor) {
        if (*cursor < 0x20 || *cursor == 0x7f) {
            clean[used++] = '?';
        } else {
            clean[used++] = (char)*cursor;
        }
    }
    clean[used] = '\0';
    waddnstr(window, clean, limit);
}

static void draw_small_terminal(void)
{
    erase();
    int rows;
    int ignored_columns;
    getmaxyx(stdscr, rows, ignored_columns);
    (void)ignored_columns;
    mvprintw(rows / 2, 2, "Terminal too small. Resize to at least %dx%d.",
             TUI_MIN_COLS, TUI_MIN_ROWS);
    refresh();
}

bool tui_start(void)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) ||
        getenv("TERM") == NULL || strcmp(getenv("TERM"), "dumb") == 0) {
        return false;
    }

    screen = newterm(NULL, stderr, stdin);
    if (screen == NULL) return false;
    set_term(screen);
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(1);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(PAIR_HEADER, COLOR_CYAN, -1);
        init_pair(PAIR_SYSTEM, COLOR_CYAN, -1);
        init_pair(PAIR_SUCCESS, COLOR_GREEN, -1);
        init_pair(PAIR_ERROR, COLOR_RED, -1);
        init_pair(PAIR_VILLAGE, COLOR_GREEN, -1);
        init_pair(PAIR_WOLVES, COLOR_RED, -1);
        init_pair(PAIR_NIGHT, COLOR_MAGENTA, -1);
        init_pair(PAIR_VOTE, COLOR_YELLOW, -1);
        init_pair(PAIR_DIM, COLOR_WHITE, -1);
    }

    saved_stdout = dup(STDOUT_FILENO);
    int null_fd = open("/dev/null", O_WRONLY);
    if (saved_stdout == -1 || null_fd == -1 ||
        dup2(null_fd, STDOUT_FILENO) == -1) {
        if (null_fd != -1) close(null_fd);
        tui_stop();
        return false;
    }
    close(null_fd);
    create_windows();
    return true;
}

void tui_stop(void)
{
    if (screen == NULL) return;
    destroy_windows();
    endwin();
    delscreen(screen);
    screen = NULL;
    if (saved_stdout != -1) {
        fflush(stdout);
        dup2(saved_stdout, STDOUT_FILENO);
        close(saved_stdout);
        saved_stdout = -1;
        clearerr(stdout);
    }
}

bool tui_is_active(void)
{
    return screen != NULL;
}

void tui_log(enum tui_log_kind kind, const char *format, ...)
{
    if (!tui_is_active()) return;
    size_t index;
    if (log_count < TUI_LOG_CAPACITY) {
        index = (log_start + log_count++) % TUI_LOG_CAPACITY;
    } else {
        index = log_start;
        log_start = (log_start + 1) % TUI_LOG_CAPACITY;
    }
    logs[index].kind = kind;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(logs[index].text, sizeof logs[index].text, format, arguments);
    va_end(arguments);
}

static void draw_header(const struct tui_view *view)
{
    werase(header_window);
    box(header_window, 0, 0);
    wattron(header_window, COLOR_PAIR(PAIR_HEADER) | A_BOLD);
    mvwprintw(header_window, 1, 2, "WEREWOLF");
    wattroff(header_window, COLOR_PAIR(PAIR_HEADER) | A_BOLD);
    mvwprintw(header_window, 1, 15, "Player: ");
    safe_add(header_window, view->username, 24);
    if (view->room_id == 0) {
        mvwprintw(header_window, 2, 2, "Lobby - create or join a room");
    } else {
        mvwprintw(header_window, 2, 2, "Room #%" PRIu32 "%s",
                  view->room_id, view->is_host ? "  [HOST]" : "");
        mvwprintw(header_window, 2, 30, "Phase: %s",
                  view->game_over ? "GAME OVER" : phase_name(view->phase));
    }
    wnoutrefresh(header_window);
}

static void draw_players(const struct tui_view *view)
{
    werase(players_window);
    box(players_window, 0, 0);
    title(players_window, "PLAYERS", PAIR_HEADER);
    int height;
    int width;
    getmaxyx(players_window, height, width);
    size_t visible = (size_t)(height - 2);
    for (size_t i = 0; i < view->player_count && i < visible; ++i) {
        const struct tui_player_view *player = &view->players[i];
        int row = (int)i + 1;
        mvwprintw(players_window, row, 2, "%-3" PRIu32 " ", player->id);
        safe_add(players_window, player->name, width > 30 ? width - 22 : 12);
        if (player->is_self) {
            wattron(players_window, A_BOLD);
            waddstr(players_window, " [YOU]");
            wattroff(players_window, A_BOLD);
        }
        if (player->is_host) waddstr(players_window, " [H]");
        if (player->alive_known) {
            int pair = player->alive ? PAIR_SUCCESS : PAIR_DIM;
            wattron(players_window, COLOR_PAIR(pair));
            mvwprintw(players_window, row, width - 8,
                      "%s", player->alive ? "ALIVE" : "DEAD");
            wattroff(players_window, COLOR_PAIR(pair));
        }
    }
    if (view->player_count > visible && visible != 0) {
        mvwprintw(players_window, height - 2, 2, "... +%zu more",
                  view->player_count - visible + 1);
    }
    wnoutrefresh(players_window);
}

static void draw_info(const struct tui_view *view)
{
    werase(info_window);
    box(info_window, 0, 0);
    title(info_window, "GAME", PAIR_HEADER);
    if (!view->game_active) {
        mvwprintw(info_window, 2, 2, "Waiting for host to start.");
        mvwprintw(info_window, 4, 2, "Commands: help | status");
    } else if (view->game_over) {
        wattron(info_window, COLOR_PAIR(PAIR_VOTE) | A_BOLD);
        mvwprintw(info_window, 2, 2, "GAME OVER");
        wattroff(info_window, COLOR_PAIR(PAIR_VOTE) | A_BOLD);
        mvwprintw(info_window, 4, 2, "Host may start a new game.");
    } else {
        mvwprintw(info_window, 1, 2, "Role: %s", role_name(view->role));
        mvwprintw(info_window, 2, 2, "Phase: %s", phase_name(view->phase));
        mvwprintw(info_window, 3, 2, "Duration: %u seconds",
                  (unsigned)view->duration_sec);
        if (view->phase == PROTOCOL_PHASE_NIGHT) {
            mvwprintw(info_window, 5, 2, "act <player_id>");
            if (view->role == PROTOCOL_ROLE_WEREWOLF)
                mvwprintw(info_window, 6, 2, "chat <message>");
        } else if (view->phase == PROTOCOL_PHASE_DAY_DISCUSS) {
            mvwprintw(info_window, 5, 2, "chat <message>");
        } else if (view->phase == PROTOCOL_PHASE_DAY_VOTE) {
            mvwprintw(info_window, 5, 2, "vote <player_id|0>");
        }
    }
    wnoutrefresh(info_window);
}

static void draw_logs(void)
{
    werase(log_window);
    box(log_window, 0, 0);
    title(log_window, "CHAT & EVENTS", PAIR_HEADER);
    int height;
    int width;
    getmaxyx(log_window, height, width);
    size_t visible = height > 2 ? (size_t)(height - 2) : 0;
    size_t first = log_count > visible ? log_count - visible : 0;
    int row = 1;
    for (size_t i = first; i < log_count; ++i, ++row) {
        const struct tui_log_entry *entry =
            &logs[(log_start + i) % TUI_LOG_CAPACITY];
        int pair = pair_for_log(entry->kind);
        wattron(log_window, COLOR_PAIR(pair) | A_BOLD);
        mvwprintw(log_window, row, 2, "[%s]", label_for_log(entry->kind));
        wattroff(log_window, COLOR_PAIR(pair) | A_BOLD);
        waddch(log_window, ' ');
        safe_add(log_window, entry->text, width - 14);
    }
    wnoutrefresh(log_window);
}

static void draw_input(const char *input)
{
    werase(input_window);
    box(input_window, 0, 0);
    title(input_window, "COMMAND", PAIR_HEADER);
    mvwaddstr(input_window, 1, 2, "> ");
    int available = getmaxx(input_window) - 6;
    safe_add(input_window, input, available);
    int cursor = (int)strlen(input);
    if (cursor > available) cursor = available;
    wmove(input_window, 1, 4 + cursor);
    wnoutrefresh(input_window);
}

void tui_draw(const struct tui_view *view, const char *input)
{
    if (!tui_is_active()) return;
    int rows;
    int columns;
    getmaxyx(stdscr, rows, columns);
    if (rows < TUI_MIN_ROWS || columns < TUI_MIN_COLS) {
        draw_small_terminal();
        return;
    }
    if (header_window == NULL || layout_rows != rows ||
        layout_columns != columns) {
        if (!create_windows()) {
            draw_small_terminal();
            return;
        }
    }
    draw_header(view);
    draw_players(view);
    draw_info(view);
    draw_logs();
    draw_input(input);
    doupdate();
}

enum tui_key tui_read_key(wchar_t *character)
{
    if (!tui_is_active()) return TUI_KEY_NONE;
    wint_t value;
    int result = get_wch(&value);
    if (result == ERR) return TUI_KEY_NONE;
    if (result == KEY_CODE_YES) {
        if (value == KEY_RESIZE) return TUI_KEY_RESIZE;
        if (value == KEY_BACKSPACE) return TUI_KEY_BACKSPACE;
        if (value == KEY_ENTER) return TUI_KEY_ENTER;
        return TUI_KEY_NONE;
    }
    if (value == 4) return TUI_KEY_EOF;
    if (value == '\n' || value == '\r') return TUI_KEY_ENTER;
    if (value == 8 || value == 127) return TUI_KEY_BACKSPACE;
    if (iswprint((wint_t)value)) {
        *character = (wchar_t)value;
        return TUI_KEY_CHARACTER;
    }
    return TUI_KEY_NONE;
}
