# ============================
#   C-ENGINE BUILD SYSTEM
# ============================

CC      = gcc
CFLAGS  = -g -O0 -pthread -Wall -Wextra -Iinclude -Ideps/blake3 \
          -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512 -DBLAKE3_USE_NEON=0

LDFLAGS =
TARGET  = engine

# Source files
SRCS = \
    src/engine.c \
    src/protocol.c \
    src/upload.c \
    src/download.c \
    src/blockstore.c \
    src/hash.c \
    src/manifest.c \
    src/locks.c \
    src/util.c \
    src/threadpool.c \
    deps/blake3/blake3.c \
    deps/blake3/blake3_dispatch.c \
    deps/blake3/blake3_portable.c

OBJS = $(SRCS:.c=.o)

# Default rule
all: $(TARGET)

# Link step
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Compile project .c files
src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Compile blake3 sources
deps/blake3/%.o: deps/blake3/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean artifacts
clean:
	rm -f $(TARGET) $(OBJS)

# Complete cleanup (optional)
distclean: clean
	rm -rf blocks/* manifests/*

.PHONY: all clean distclean
