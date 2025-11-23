// src/blockstore.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "engine_config.h"
#include "blockstore.h"
#include "util.h"
#include "protocol.h"


// Global blockstore root directory used by all blockstore operations (put/get/exists).
// If blockstore_init is called with root_dir == NULL, ENGINE_BLOCKS_DIR is used as default.
static char g_blockstore_root[ENGINE_MAX_PATH_LEN] = ENGINE_BLOCKS_DIR;

/**
 * Initialize the blockstore root directory.
 *
 * If root_dir is NULL or empty, ENGINE_BLOCKS_DIR is used instead.
 * The directory is created recursively (mkdir -p behavior).
 *
 * Returns:
 *   0  on success
 *  -1  on error (e.g., invalid path or mkdir failure)
 */
int blockstore_init(const char* root_dir) {
    const char* dir = root_dir;

    // Fallback to default blockstore directory
    if (dir == NULL || dir[0] == '\0') {
        dir = ENGINE_BLOCKS_DIR;
    }

    // Ensure the path fits into the global buffer
    size_t len = strlen(dir);
    if (len >= sizeof(g_blockstore_root)) {
        log_error("[BLOCKSTORE] root path too long: %s", dir);
        return -1;
    }

    // Store the chosen directory path in the global variable
    memcpy(g_blockstore_root, dir, len + 1);  // include null terminator

    // Create the directory recursively (mkdir -p)
    if (util_mkdir_p(g_blockstore_root) < 0) {
        log_error("[BLOCKSTORE] failed to create blockstore root: %s", g_blockstore_root);
        return -1;
    }

    log_info("[BLOCKSTORE] initialized at root: %s", g_blockstore_root);
    return 0;
}



/**
 * Construct the full filesystem path for a block given its hash.
 *
 * Sharding layout:
 *   <root>/ab/cd/<hash_str>
 *
 * Where:
 *   ab = first 2 chars of hash_str
 *   cd = next 2 chars of hash_str
 *
 * out_path must have at least ENGINE_MAX_PATH_LEN bytes.
 *
 * Returns:
 *   0  on success
 *  -1  on invalid hash or path error
 */
 static int blockstore_make_path(const char* hash_str, char* out_path) {
    if (!hash_str || strlen(hash_str) < 4) {
        return -1;  // hash too short for sharding
    }

    // Extract two sharding levels
    char sub1[3] = { hash_str[0], hash_str[1], '\0' };
    char sub2[3] = { hash_str[2], hash_str[3], '\0' };

    // Temporary buffers for intermediate paths
    char path1[ENGINE_MAX_PATH_LEN];
    char path2[ENGINE_MAX_PATH_LEN];

    // root/sub1
    if (util_join_path(g_blockstore_root, sub1, path1, sizeof(path1)) < 0)
        return -1;

    // root/sub1/sub2
    if (util_join_path(path1, sub2, path2, sizeof(path2)) < 0)
        return -1;

    // root/sub1/sub2/hash_str
    if (util_join_path(path2, hash_str, out_path, ENGINE_MAX_PATH_LEN) < 0)
        return -1;

    return 0;
}


int blockstore_put(const char* hash_str, const uint8_t* data, size_t len) {
    if (!hash_str) {
        log_error("[BLOCKSTORE] blockstore_put called with NULL hash_str");
        return -1;
    }
    if (len > 0 && data == NULL) {
        log_error("[BLOCKSTORE] blockstore_put called with NULL data and len=%zu", len);
        return -1;
    }

    size_t hlen = strlen(hash_str);
    if (hlen < 4) {
        log_error("[BLOCKSTORE] hash_str too short for sharding: %s", hash_str);
        return -1;
    }

    // If block already exists, treat as success (idempotent put)
    int exists = blockstore_exists(hash_str);
    if (exists == 1) {
        log_info("[BLOCKSTORE] block already exists, skipping write: %s", hash_str);
        return 0;
    } else if (exists < 0) {
        log_error("[BLOCKSTORE] blockstore_exists failed for: %s", hash_str);
        return -1;
    }

    // Extract shard prefixes: ab and cd
    char sub1[3] = { hash_str[0], hash_str[1], '\0' };
    char sub2[3] = { hash_str[2], hash_str[3], '\0' };

    char path1[ENGINE_MAX_PATH_LEN];
    char path2[ENGINE_MAX_PATH_LEN];

    // Build and create: <root>/ab
    if (util_join_path(g_blockstore_root, sub1, path1, sizeof(path1)) < 0) {
        log_error("[BLOCKSTORE] failed to join path for first shard: %s", sub1);
        return -1;
    }
    if (util_mkdir_p(path1) < 0) {
        log_error("[BLOCKSTORE] failed to create shard dir: %s", path1);
        return -1;
    }

    // Build and create: <root>/ab/cd
    if (util_join_path(path1, sub2, path2, sizeof(path2)) < 0) {
        log_error("[BLOCKSTORE] failed to join path for second shard: %s", sub2);
        return -1;
    }
    if (util_mkdir_p(path2) < 0) {
        log_error("[BLOCKSTORE] failed to create shard dir: %s", path2);
        return -1;
    }

    // Build final path: <root>/ab/cd/<hash_str>
    char full_path[ENGINE_MAX_PATH_LEN];
    if (blockstore_make_path(hash_str, full_path) < 0) {
        log_error("[BLOCKSTORE] failed to construct full path for hash: %s", hash_str);
        return -1;
    }

    // Build temporary path: <full_path>.tmp.<pid>
    char tmp_path[ENGINE_MAX_PATH_LEN];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld", full_path, (long)getpid());
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
        log_error("[BLOCKSTORE] temp path too long for hash: %s", hash_str);
        return -1;
    }

    // Open temp file for writing
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        log_error("[BLOCKSTORE] failed to open temp file %s: %s", tmp_path, strerror(errno));
        return -1;
    }

    // Write all data (simple loop, similar to write_all)
    size_t written = 0;
    while (written < len) {
        ssize_t w = write(fd, data + written, len - written);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_error("[BLOCKSTORE] write failed for %s: %s", tmp_path, strerror(errno));
            close(fd);
            unlink(tmp_path);
            return -1;
        }
        written += (size_t)w;
    }

    // Ensure data is flushed to disk
    if (fsync(fd) < 0) {
        log_error("[BLOCKSTORE] fsync failed for %s: %s", tmp_path, strerror(errno));
        close(fd);
        unlink(tmp_path);
        return -1;
    }

    if (close(fd) < 0) {
        log_error("[BLOCKSTORE] close failed for %s: %s", tmp_path, strerror(errno));
        unlink(tmp_path);
        return -1;
    }

    // Atomic rename to final path
    if (rename(tmp_path, full_path) < 0) {
        log_error("[BLOCKSTORE] rename(%s -> %s) failed: %s",
                  tmp_path, full_path, strerror(errno));
        unlink(tmp_path);
        return -1;
    }

    log_info("[BLOCKSTORE] stored block %s at %s (len=%zu)", hash_str, full_path, len);
    return 0;
}

int blockstore_get(const char* hash_str, uint8_t** out_data, size_t* out_len) {
    if (!hash_str || !out_data || !out_len) {
        log_error("[BLOCKSTORE] blockstore_get: NULL argument");
        return -1;
    }

    *out_data = NULL;
    *out_len  = 0;

    // Build full path: <root>/ab/cd/<hash_str>
    char full_path[ENGINE_MAX_PATH_LEN];
    if (blockstore_make_path(hash_str, full_path) < 0) {
        log_error("[BLOCKSTORE] blockstore_get: failed to build path for hash %s", hash_str);
        return -1;
    }

    // Open file for reading
    int fd = open(full_path, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            log_info("[BLOCKSTORE] block not found: %s", hash_str);
            return -1;
        }
        log_error("[BLOCKSTORE] open failed for %s: %s", full_path, strerror(errno));
        return -1;
    }

    // Get file size
    struct stat st;
    if (fstat(fd, &st) < 0) {
        log_error("[BLOCKSTORE] fstat failed for %s: %s", full_path, strerror(errno));
        close(fd);
        return -1;
    }

    if (st.st_size < 0) {
        log_error("[BLOCKSTORE] invalid file size for %s", full_path);
        close(fd);
        return -1;
    }

    *out_len = (size_t)st.st_size;

    // Allocate buffer
    uint8_t* buf = (uint8_t*)malloc(*out_len);
    if (!buf) {
        log_error("[BLOCKSTORE] malloc failed for size %zu", *out_len);
        close(fd);
        return -1;
    }

    // Read file fully
    ssize_t r = read_n(fd, buf, *out_len);
    if (r <= 0) {
        log_error("[BLOCKSTORE] read_n failed for %s", full_path);
        free(buf);
        close(fd);
        return -1;
    }

    if ((size_t)r != *out_len) {
        log_error("[BLOCKSTORE] partial read for %s", full_path);
        free(buf);
        close(fd);
        return -1;
    }

    close(fd);

    // Success
    *out_data = buf;
    return 0;
}

int blockstore_exists(const char* hash_str) {
    if (!hash_str) {
        log_error("[BLOCKSTORE] blockstore_exists called with NULL hash_str");
        return -1;
    }

    char path[ENGINE_MAX_PATH_LEN];
    if (blockstore_make_path(hash_str, path) < 0) {
        log_error("[BLOCKSTORE] blockstore_exists: failed to build path for hash %s", hash_str);
        return -1;
    }

    // 1 = exists, 0 = does not exist
    if (access(path, F_OK) == 0) {
        return 1;
    }
    return 0;
}
