CC = gcc
CPPFLAGS += -Iinclude
CFLAGS ?= -std=c11 -Wall -Wextra
BUILD_DIR := build
COMMON_SOURCE := src/common/protocol.c
COMMON_HEADERS := include/net_config.h include/protocol.h
GAME_TEST := $(BUILD_DIR)/test_game
GAME_NIGHT_TEST := $(BUILD_DIR)/test_game_night
GAME_WIN_TEST := $(BUILD_DIR)/test_game_win
GAME_DAY_TEST := $(BUILD_DIR)/test_game_day

.PHONY: all clean test-game

all: $(BUILD_DIR)/server $(BUILD_DIR)/client

$(BUILD_DIR)/server: src/server/main.c src/server/session.c include/server/session.h src/server/lobby.c include/server/lobby.h src/server/game.c include/server/game.h $(COMMON_SOURCE) $(COMMON_HEADERS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

$(BUILD_DIR)/client: src/client/main.c src/client/lobby.c include/client/lobby.h $(COMMON_SOURCE) $(COMMON_HEADERS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

$(GAME_TEST): tests/game.c src/server/game.c include/server/game.h include/protocol.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

$(GAME_NIGHT_TEST): tests/game_night.c src/server/game.c include/server/game.h include/protocol.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

$(GAME_WIN_TEST): tests/game_win.c src/server/game.c include/server/game.h include/protocol.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

$(GAME_DAY_TEST): tests/game_day.c src/server/game.c include/server/game.h include/protocol.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

test-game: $(GAME_TEST) $(GAME_NIGHT_TEST) $(GAME_WIN_TEST) $(GAME_DAY_TEST)
	./$(GAME_TEST)
	./$(GAME_NIGHT_TEST)
	./$(GAME_WIN_TEST)
	./$(GAME_DAY_TEST)

$(BUILD_DIR):
	mkdir -p $@

clean:
	$(RM) $(BUILD_DIR)/server $(BUILD_DIR)/client $(GAME_TEST) $(GAME_NIGHT_TEST) $(GAME_WIN_TEST) $(GAME_DAY_TEST)
