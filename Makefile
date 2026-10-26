# Build trong Ubuntu/WSL: make, make test, make sanitize, make gui-check.
CC = gcc
PKG_CONFIG ?= pkg-config
BUILD_DIR ?= build
BIN_DIR ?= bin
CFLAGS ?= -O2 -g
WARN_FLAGS = -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
             -Wstrict-prototypes -Wmissing-prototypes -Werror
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Icommon/include -Iserver/include \
            $(shell $(PKG_CONFIG) --cflags libcjson)
JSON_LIBS = $(shell $(PKG_CONFIG) --libs libcjson)
COMMON_SRC = common/src/frame.c common/src/protocol.c common/src/net.c
COMMON_OBJ = $(COMMON_SRC:%.c=$(BUILD_DIR)/%.o)
SERVER_OBJ = $(BUILD_DIR)/server/src/main.o $(BUILD_DIR)/server/src/server.o
CLIENT_OBJ = $(BUILD_DIR)/client/src/main.o
TEST_NAMES = test_frame test_protocol test_net
TEST_BIN = $(TEST_NAMES:%=$(BIN_DIR)/%)
.SECONDARY: $(TEST_NAMES:%=$(BUILD_DIR)/tests/%.o)

.PHONY: all test unit-test sanitize gui-smoke gui-check clean
all: $(BIN_DIR)/ltm-server $(BIN_DIR)/ltm-client

$(BIN_DIR)/ltm-server: $(SERVER_OBJ) $(COMMON_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $^ $(JSON_LIBS) -o $@

$(BIN_DIR)/ltm-client: $(CLIENT_OBJ) $(COMMON_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $^ $(JSON_LIBS) -o $@

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN_FLAGS) -MMD -MP -c $< -o $@

$(BIN_DIR)/test_%: $(BUILD_DIR)/tests/test_%.o $(COMMON_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $^ $(JSON_LIBS) -o $@

unit-test: $(TEST_BIN)
	$(BIN_DIR)/test_frame
	$(BIN_DIR)/test_protocol
	$(BIN_DIR)/test_net

test: all unit-test
	python3 tests/integration.py --server $(BIN_DIR)/ltm-server --client $(BIN_DIR)/ltm-client

sanitize:
	$(MAKE) BUILD_DIR=build/sanitize BIN_DIR=bin/sanitize \
	  CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' \
	  LDFLAGS='-fsanitize=address,undefined' test

$(BIN_DIR)/ltm-gui-smoke: client/src/gui_smoke.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(WARN_FLAGS) $(shell $(PKG_CONFIG) --cflags gtk+-3.0) $< \
	  $(LDFLAGS) $(shell $(PKG_CONFIG) --libs gtk+-3.0) -o $@

gui-smoke: $(BIN_DIR)/ltm-gui-smoke
gui-check: gui-smoke
	$(BIN_DIR)/ltm-gui-smoke --check

clean:
	rm -f $(BIN_DIR)/ltm-server $(BIN_DIR)/ltm-client $(BIN_DIR)/ltm-gui-smoke $(TEST_BIN)
	rm -f $(COMMON_OBJ) $(SERVER_OBJ) $(CLIENT_OBJ) \
	  $(COMMON_OBJ:.o=.d) $(SERVER_OBJ:.o=.d) $(CLIENT_OBJ:.o=.d) \
	  $(TEST_NAMES:%=$(BUILD_DIR)/tests/%.o) $(TEST_NAMES:%=$(BUILD_DIR)/tests/%.d)

-include $(COMMON_OBJ:.o=.d) $(SERVER_OBJ:.o=.d) $(CLIENT_OBJ:.o=.d) \
         $(TEST_NAMES:%=$(BUILD_DIR)/tests/%.d)
