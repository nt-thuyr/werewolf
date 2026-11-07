CC = gcc
CPPFLAGS += -Iinclude
CFLAGS ?= -std=c11 -Wall -Wextra
BUILD_DIR := build
COMMON_SOURCE := src/common/protocol.c
COMMON_HEADERS := include/net_config.h include/protocol.h

.PHONY: all clean

all: $(BUILD_DIR)/server $(BUILD_DIR)/client

$(BUILD_DIR)/server: src/server/main.c src/server/session.c src/server/session.h $(COMMON_SOURCE) $(COMMON_HEADERS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

$(BUILD_DIR)/client: src/client/main.c $(COMMON_SOURCE) $(COMMON_HEADERS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

$(BUILD_DIR):
	mkdir -p $@

clean:
	$(RM) $(BUILD_DIR)/server $(BUILD_DIR)/client
