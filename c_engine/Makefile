CC = gcc
CFLAGS = -O2 -Wall -Wextra -Iinclude
LDFLAGS =
SRC = src/c_engine.c
BIN = engine
BUILD_DIR = build

$(BUILD_DIR)/$(BIN): $(SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SRC) -o $(BUILD_DIR)/$(BIN) $(LDFLAGS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)
