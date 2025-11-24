// src/manifest.c
// Manifest handling: in-memory manifest structure, JSON save/load, and CID generation.

#define _GNU_SOURCE  // needed for open_memstream on GNU systems

#include <stdio.h>      // FILE, fprintf, fopen, etc.
#include <stdlib.h>     // malloc, free, calloc
#include <string.h>     // strlen, strdup, memcpy, memset, strstr
#include <stdint.h>     // uint32_t, uint64_t, uint8_t
#include <sys/stat.h>   // stat, fstat
#include <sys/types.h>  // general types
#include <fcntl.h>      // open flags
#include <unistd.h>     // close, read, write, fsync
#include <errno.h>      // errno, strerror

#include "engine_config.h"  // ENGINE_MANIFEST_DIR, ENGINE_MAX_PATH_LEN, etc.
#include "manifest.h"       // manifest / manifest_chunk declarations
#include "hash.h"           // hash_compute, hash_to_multihash_b32, hash_result_t
#include "util.h"           // util_join_path, util_mkdir_p, logging helpers


// Build full path for a manifest file: ENGINE_MANIFEST_DIR/<cid>.json
// out_path must have at least out_size bytes.
// Returns 0 on success, <0 on error.
static int manifest_make_path(const char* cid, char* out_path, size_t out_size) {
    if (!cid || !out_path) {
        return -1;
    }

    // Build filename "<cid>.json"
    char filename[256];
    int n = snprintf(filename, sizeof(filename), "%s.json", cid);
    if (n < 0 || (size_t)n >= sizeof(filename)) {
        // formatted string too long
        return -1;
    }

    // Join: ENGINE_MANIFEST_DIR + filename
    if (util_join_path(ENGINE_MANIFEST_DIR, filename, out_path, out_size) < 0) {
        return -1;
    }

    return 0;
}
// Create a new manifest object in memory.
// - filename: original file name (may be NULL)
// - chunk_size: chunk size used when splitting the file
// - hash_algo: name of hash algorithm (e.g., "blake3"); if NULL, defaults to "blake3"
//
// Returns pointer to allocated manifest on success, NULL on allocation failure.
manifest* manifest_create(const char* filename, uint32_t chunk_size, const char* hash_algo) {
    manifest* m = (manifest*)calloc(1, sizeof(*m));
    if (!m) {
        return NULL;
    }

    m->version     = 1;
    m->chunk_size  = chunk_size;
    m->total_size  = 0;
    m->chunk_count = 0;
    m->chunks      = NULL;

    // Store filename (if provided)
    if (filename) {
        m->filename = strdup(filename);
        if (!m->filename) {
            manifest_free(m);
            return NULL;
        }
    }

    // Store hash algorithm string (default to "blake3" if not provided)
    const char* algo_str = hash_algo ? hash_algo : "blake3";
    m->hash_algo = strdup(algo_str);
    if (!m->hash_algo) {
        manifest_free(m);
        return NULL;
    }

    return m;
}

// Add a new chunk entry to the manifest.
// - index: chunk index (0,1,2,...)
// - size:  size of the chunk in bytes
// - hash_str: multihash base32 string for this chunk
//
// Returns 0 on success, <0 on error.
int manifest_add_chunk(manifest* m, uint32_t index, uint32_t size, const char* hash_str) {
    if (!m || !hash_str) {
        return -1;
    }

    uint32_t new_count = m->chunk_count + 1;

    manifest_chunk* new_chunks = realloc(m->chunks, new_count * sizeof(manifest_chunk));
    if (!new_chunks) {
        return -1;
    }

    m->chunks = new_chunks;

    manifest_chunk* c = &m->chunks[m->chunk_count];
    c->index = index;
    c->size  = size;
    c->hash_str = strdup(hash_str);

    if (!c->hash_str) {
        return -1;
    }

    m->chunk_count = new_count;

    return 0;
}

// Comparator used for qsort to order chunks by their index (ascending).
static int cmp_chunk_index(const void* a, const void* b) {
    const manifest_chunk* ca = (const manifest_chunk*)a;
    const manifest_chunk* cb = (const manifest_chunk*)b;

    if (ca->index < cb->index) return -1;
    if (ca->index > cb->index) return 1;
    return 0;
}

// Finalize manifest after all chunks have been added.
// - sets the total_size
// - optionally sorts chunks by index so they are in deterministic order
void manifest_finalize(manifest* m, uint64_t total_size) {
    if (!m) {
        return;
    }

    // Store total file size
    m->total_size = total_size;

    // Sort chunks by index so that [0..chunk_count-1] is in ascending order
    if (m->chunk_count > 1 && m->chunks) {
        qsort(m->chunks,
              m->chunk_count,
              sizeof(manifest_chunk),
              cmp_chunk_index);
    }
}

int manifest_save_and_get_cid(const manifest* m, char** out_cid) {
    if (!m || !out_cid) {
        return -1;
    }
    *out_cid = NULL;

    //
    // 1) Build JSON in-memory using open_memstream
    //
    char*  json_buf = NULL;
    size_t json_len = 0;

    FILE* mem = open_memstream(&json_buf, &json_len);
    if (!mem) {
        return -1;
    }

    // JSON header and basic fields
    fprintf(mem,
        "{\n"
        "  \"version\": %u,\n"
        "  \"hash_algo\": \"%s\",\n"
        "  \"chunk_size\": %u,\n"
        "  \"total_size\": %llu,\n"
        "  \"filename\": \"%s\",\n"
        "  \"chunks\": [\n",
        m->version,
        m->hash_algo ? m->hash_algo : "blake3",
        m->chunk_size,
        (unsigned long long)m->total_size,
        m->filename ? m->filename : ""
    );

    // JSON array of chunks
    for (uint32_t i = 0; i < m->chunk_count; ++i) {
        const manifest_chunk* c = &m->chunks[i];

        fprintf(mem,
            "    { \"index\": %u, \"size\": %u, \"hash\": \"%s\" }%s\n",
            c->index,
            c->size,
            c->hash_str ? c->hash_str : "",
            (i + 1 < m->chunk_count) ? "," : ""
        );
    }

    fprintf(mem, "  ]\n}\n");

    // Close memstream so that json_buf/json_len become valid
    fclose(mem);

    if (!json_buf || json_len == 0) {
        free(json_buf);
        return -1;
    }

    //
    // 2) Compute hash over JSON and convert to a (fake) multihash "CID"
    //
    hash_result_t h;
    memset(&h, 0, sizeof(h));

    // Use current algorithm from environment (for now always fake BLAKE3)
    hash_algo_t algo = hash_algo_from_env();
    if (hash_compute(algo,
                     (const uint8_t*)json_buf,
                     json_len,
                     &h) < 0) {
        free(json_buf);
        return -1;
    }

    char* cid = NULL;
    if (hash_to_multihash_b32(&h, &cid) < 0) {
        hash_result_free(&h);
        free(json_buf);
        return -1;
    }

    // We no longer need the raw digest struct
    hash_result_free(&h);

    //
    // 3) Atomically write JSON to manifests/<cid>.json
    //
    // Ensure manifests directory exists
    if (util_mkdir_p(ENGINE_MANIFEST_DIR) < 0) {
        free(json_buf);
        free(cid);
        return -1;
    }

    char path[ENGINE_MAX_PATH_LEN];
    if (manifest_make_path(cid, path, sizeof(path)) < 0) {
        free(json_buf);
        free(cid);
        return -1;
    }

    // Build temporary path: <cid>.json.tmp.<pid>
    char tmp_path[ENGINE_MAX_PATH_LEN];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld",
                     path, (long)getpid());
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
        free(json_buf);
        free(cid);
        return -1;
    }

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        free(json_buf);
        free(cid);
        return -1;
    }

    // Write entire JSON buffer
    size_t written = 0;
    while (written < json_len) {
        ssize_t w = write(fd, json_buf + written, json_len - written);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            unlink(tmp_path);
            free(json_buf);
            free(cid);
            return -1;
        }
        written += (size_t)w;
    }

    if (fsync(fd) < 0) {
        close(fd);
        unlink(tmp_path);
        free(json_buf);
        free(cid);
        return -1;
    }

    if (close(fd) < 0) {
        unlink(tmp_path);
        free(json_buf);
        free(cid);
        return -1;
    }

    // Atomic rename to final manifest path
    if (rename(tmp_path, path) < 0) {
        unlink(tmp_path);
        free(json_buf);
        free(cid);
        return -1;
    }

    // We don't need JSON buffer anymore
    free(json_buf);

    // Success: return CID string to caller
    *out_cid = cid;
    return 0;
}



// Helper: read entire file into memory (NUL-terminated)
// Returns 0 on success, <0 on error.
static int manifest_read_file(const char* path, char** out_buf, size_t* out_len) {
    *out_buf = NULL;
    *out_len = 0;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        log_error("[MANIFEST] open failed for %s: %s", path, strerror(errno));
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        log_error("[MANIFEST] fstat failed for %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }

    if (st.st_size < 0) {
        log_error("[MANIFEST] invalid size for %s", path);
        close(fd);
        return -1;
    }

    size_t len = (size_t)st.st_size;
    char* buf = (char*)malloc(len + 1); // +1 for '\0'
    if (!buf) {
        log_error("[MANIFEST] malloc failed for %s", path);
        close(fd);
        return -1;
    }

    size_t off = 0;
    while (off < len) {
        ssize_t r = read(fd, buf + off, len - off);
        if (r < 0) {
            if (errno == EINTR) continue;
            log_error("[MANIFEST] read failed for %s: %s", path, strerror(errno));
            free(buf);
            close(fd);
            return -1;
        }
        if (r == 0) break; // EOF
        off += (size_t)r;
    }
    close(fd);

    buf[off] = '\0';
    *out_buf = buf;
    *out_len = off;
    return 0;
}

// Helper: extract string field: "key": "value"
static int manifest_extract_string(const char* json,
                                   const char* key,
                                   char* out_buf,
                                   size_t out_size) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char* p = strstr(json, pattern);
    if (!p) return -1;

    // Move to first double-quote after colon
    p = strchr(p, ':');
    if (!p) return -1;
    p = strchr(p, '"');
    if (!p) return -1;
    p++; // after opening quote

    const char* end = strchr(p, '"');
    if (!end) return -1;

    size_t len = (size_t)(end - p);
    if (len >= out_size) return -1;

    memcpy(out_buf, p, len);
    out_buf[len] = '\0';
    return 0;
}

// Helper: extract unsigned long long field: "key": 12345
static int manifest_extract_ull(const char* json,
                                const char* key,
                                unsigned long long* out_val) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char* p = strstr(json, pattern);
    if (!p) return -1;

    p = strchr(p, ':');
    if (!p) return -1;
    p++; // after ':'

    // Skip spaces
    while (*p == ' ' || *p == '\t') p++;

    unsigned long long v = 0;
    if (sscanf(p, "%llu", &v) != 1) {
        return -1;
    }
    *out_val = v;
    return 0;
}

// Helper: extract uint32 field (به صورت ساده از manifest_extract_ull استفاده می‌کنیم)
static int manifest_extract_u32(const char* json,
                                const char* key,
                                uint32_t* out_val) {
    unsigned long long v = 0;
    if (manifest_extract_ull(json, key, &v) < 0) {
        return -1;
    }
    *out_val = (uint32_t)v;
    return 0;
}

/**
 * Load manifest from disk given a CID.
 *
 * This assumes the JSON format produced by manifest_save_and_get_cid().
 *
 * Returns:
 *   manifest* on success (caller must call manifest_free),
 *   NULL on error.
 */
manifest* manifest_load_from_cid(const char* cid) {
    if (!cid) {
        log_error("[MANIFEST] manifest_load_from_cid called with NULL cid");
        return NULL;
    }

    char path[ENGINE_MAX_PATH_LEN];
    if (manifest_make_path(cid, path, sizeof(path)) < 0) {
        log_error("[MANIFEST] failed to build path for cid=%s", cid);
        return NULL;
    }

    char* json_buf = NULL;
    size_t json_len = 0;
    if (manifest_read_file(path, &json_buf, &json_len) < 0) {
        // error already logged
        return NULL;
    }

    // 1) Extract basic fields: filename, chunk_size, total_size, hash_algo
    char filename[256];
    char hash_algo[64];
    uint32_t chunk_size = 0;
    unsigned long long total_size_ull = 0;

    if (manifest_extract_string(json_buf, "filename", filename, sizeof(filename)) < 0) {
        log_error("[MANIFEST] failed to parse filename from %s", path);
        free(json_buf);
        return NULL;
    }

    if (manifest_extract_string(json_buf, "hash_algo", hash_algo, sizeof(hash_algo)) < 0) {
        // default to blake3 if not found
        strcpy(hash_algo, "blake3");
    }

    if (manifest_extract_u32(json_buf, "chunk_size", &chunk_size) < 0) {
        log_error("[MANIFEST] failed to parse chunk_size from %s", path);
        free(json_buf);
        return NULL;
    }

    if (manifest_extract_ull(json_buf, "total_size", &total_size_ull) < 0) {
        log_error("[MANIFEST] failed to parse total_size from %s", path);
        free(json_buf);
        return NULL;
    }

    // 2) Create manifest struct
    manifest* m = manifest_create(filename, chunk_size, hash_algo);
    if (!m) {
        log_error("[MANIFEST] manifest_create failed while loading %s", path);
        free(json_buf);
        return NULL;
    }

    // 3) Parse chunks array
    const char* p = strstr(json_buf, "\"chunks\"");
    if (!p) {
        log_error("[MANIFEST] no \"chunks\" array in %s", path);
        manifest_free(m);
        free(json_buf);
        return NULL;
    }

    p = strchr(p, '[');
    if (!p) {
        log_error("[MANIFEST] malformed chunks array in %s", path);
        manifest_free(m);
        free(json_buf);
        return NULL;
    }

    // Move after '['
    p++;

    while (1) {
        // Find next '{'
        const char* obj_start = strchr(p, '{');
        if (!obj_start) {
            break; // no more chunks
        }

        const char* obj_end = strchr(obj_start, '}');
        if (!obj_end) {
            log_error("[MANIFEST] unterminated chunk object in %s", path);
            manifest_free(m);
            free(json_buf);
            return NULL;
        }

        // Copy chunk JSON into a small buffer for sscanf
        size_t obj_len = (size_t)(obj_end - obj_start + 1);
        if (obj_len >= 512) {
            log_error("[MANIFEST] chunk object too large in %s", path);
            manifest_free(m);
            free(json_buf);
            return NULL;
        }

        char chunk_json[512];
        memcpy(chunk_json, obj_start, obj_len);
        chunk_json[obj_len] = '\0';

        uint32_t idx = 0;
        uint32_t size = 0;
        char hash_str[256];

        // Expected format: { "index": %u, "size": %u, "hash": "..." }
        int matched = sscanf(chunk_json,
                             " { \"index\" : %u , \"size\" : %u , \"hash\" : \"%255[^\"]\"",
                             &idx, &size, hash_str);
        if (matched != 3) {
            matched = sscanf(chunk_json,
                             " { \"index\": %u, \"size\": %u, \"hash\": \"%255[^\"]\"",
                             &idx, &size, hash_str);
        }

        if (matched != 3) {
            log_error("[MANIFEST] failed to parse chunk object: %s", chunk_json);
            manifest_free(m);
            free(json_buf);
            return NULL;
        }

        if (manifest_add_chunk(m, idx, size, hash_str) < 0) {
            log_error("[MANIFEST] manifest_add_chunk failed while loading %s", path);
            manifest_free(m);
            free(json_buf);
            return NULL;
        }

        // Continue after this object
        p = obj_end + 1;
    }

    // 4) Finalize manifest (sort by index + set total_size)
    manifest_finalize(m, (uint64_t)total_size_ull);

    free(json_buf);
    return m;
}

/**
 * Free all memory associated with a manifest structure.
 *
 * This function frees:
 *  - filename string
 *  - hash_algo string
 *  - each chunk.hash_str
 *  - the chunks array
 *  - the manifest struct itself
 */
 void manifest_free(manifest* m) {
    if (!m) return;

    // Free filename
    if (m->filename) {
        free(m->filename);
        m->filename = NULL;
    }

    // Free hash algorithm string
    if (m->hash_algo) {
        free(m->hash_algo);
        m->hash_algo = NULL;
    }

    // Free chunks array
    if (m->chunks) {
        for (uint32_t i = 0; i < m->chunk_count; ++i) {
            free(m->chunks[i].hash_str);
            m->chunks[i].hash_str = NULL;
        }
        free(m->chunks);
        m->chunks = NULL;
    }

    // Finally free the manifest struct
    free(m);
}
