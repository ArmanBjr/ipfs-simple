# ============================
#   C-ENGINE BUILD SYSTEM
# ============================

CC      = gcc
CFLAGS  = -O2 -pthread -Wall -Wextra -Iinclude
LDFLAGS =
TARGET  = c_engine

# Source files
SRCS = \
    src/c_engine.c \
    src/protocol.c \
    src/upload.c \
    src/download.c \
    src/blockstore.c \
    src/hash.c \
    src/manifest.c \
    src/locks.c \
    src/util.c \
    src/threadpool.c

OBJS = $(SRCS:.c=.o)

# Default rule
all: $(TARGET)

# Link step
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Compile each .c to .o
src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean artifacts
clean:
	rm -f $(TARGET) $(OBJS)

# Complete cleanup (optional)
distclean: clean
	rm -rf blocks/* manifests/*

.PHONY: all clean distclean
