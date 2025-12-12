#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "engine_config.h"
#include "manifest.h"
#include "hash.h"
#include "util.h"
#include "locks.h"
#include "blockstore.h"

struct manifest* manifest_parse_from_json(const char* json_buf);

static int manifest_make_path(const char* cid, char* out_path, size_t out_size) {
    if (!cid || !out_path) {
        return -1;
    }

    char filename[256];
    int n = snprintf(filename, sizeof(filename), "%s.json", cid);
    if (n < 0 || (size_t)n >= sizeof(filename)) {
        return -1;
    }

    if (util_join_path(ENGINE_MANIFEST_DIR, filename, out_path, out_size) < 0) {
        return -1;
    }

    return 0;
}

manifest* manifest_create(const char* filename, uint32_t chunk_size, const char* hash_algo) {
    manifest* m = (manifest*)calloc(1, sizeof(*m));
    if (!m) {
        return NULL;
    }

    m->version = 1;
    m->chunk_size = chunk_size;
    m->total_size = 0;
    m->chunk_count = 0;
    m->chunks = NULL;

    if (filename) {
        m->filename = strdup(filename);
        if (!m->filename) {
            manifest_free(m);
            return NULL;
        }
    }

    const char* algo_str = hash_algo ? hash_algo : "blake3";
    m->hash_algo = strdup(algo_str);
    if (!m->hash_algo) {
        manifest_free(m);
        return NULL;
    }

    return m;
}

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
    c->size = size;
    c->hash_str = strdup(hash_str);

    if (!c->hash_str) {
        return -1;
    }

    m->chunk_count = new_count;

    return 0;
}

static int cmp_chunk_index(const void* a, const void* b) {
    const manifest_chunk* ca = (const manifest_chunk*)a;
    const manifest_chunk* cb = (const manifest_chunk*)b;

    if (ca->index < cb->index) return -1;
    if (ca->index > cb->index) return 1;
    return 0;
}

void manifest_finalize(manifest* m, uint64_t total_size) {
    if (!m) {
        return;
    }

    m->total_size = total_size;

    if (m->chunk_count > 1 && m->chunks) {
        qsort(m->chunks, m->chunk_count, sizeof(manifest_chunk), cmp_chunk_index);
    }
}

int manifest_save_and_get_cid(const manifest* m, char** out_cid) {
    if (!m || !out_cid) {
        return -1;
    }
    *out_cid = NULL;

    pthread_rwlock_wrlock(&g_manifest_lock);

    char* json_buf = NULL;
    size_t json_len = 0;
    FILE* mem = open_memstream(&json_buf, &json_len);
    if (!mem) {
        pthread_rwlock_unlock(&g_manifest_lock);
        return -1;
    }

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

    fclose(mem);
    if (!json_buf || json_len == 0) {
        free(json_buf);
        pthread_rwlock_unlock(&g_manifest_lock);
        return -1;
    }

    hash_result_t h;
    memset(&h, 0, sizeof(h));
    hash_algo_t algo = hash_algo_from_env();
    if (hash_compute(algo, (const uint8_t*)json_buf, json_len, &h) < 0) {
        free(json_buf);
        pthread_rwlock_unlock(&g_manifest_lock);
        return -1;
    }

    char* cid = NULL;
    if (hash_to_multihash_b32(&h, &cid) < 0) {
        hash_result_free(&h);
        free(json_buf);
        pthread_rwlock_unlock(&g_manifest_lock);
        return -1;
    }
    hash_result_free(&h);

    for (uint32_t i = 0; i < m->chunk_count; ++i) {
        const char* hash = m->chunks[i].hash_str;
        if (!hash) continue;

        char ref_path[ENGINE_MAX_PATH_LEN];
        snprintf(ref_path, sizeof(ref_path), "%s/%s.ref", ENGINE_BLOCKS_DIR, hash);

        FILE* f = fopen(ref_path, "r+");
        int refcount = 0;
        if (f) {
            if (fscanf(f, "%d", &refcount) != 1) {
                refcount = 0;
            }
            rewind(f);
        } else {
            f = fopen(ref_path, "w");
            if (!f) {
                log_error("[MANIFEST] failed to open ref file: %s", ref_path);
                free(json_buf);
                free(cid);
                pthread_rwlock_unlock(&g_manifest_lock);
                return -1;
            }
        }
        refcount++;
        fprintf(f, "%d\n", refcount);
        fclose(f);
    }

    if (util_mkdir_p(ENGINE_MANIFEST_DIR) < 0) {
        free(json_buf);
        free(cid);
        pthread_rwlock_unlock(&g_manifest_lock);
        return -1;
    }

    char path[ENGINE_MAX_PATH_LEN];
    if (manifest_make_path(cid, path, sizeof(path)) < 0) {
        free(json_buf);
        free(cid);
        pthread_rwlock_unlock(&g_manifest_lock);
        return -1;
    }

    char tmp_path[ENGINE_MAX_PATH_LEN];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld", path, (long)getpid());

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        free(json_buf);
        free(cid);
        pthread_rwlock_unlock(&g_manifest_lock);
        return -1;
    }

    size_t written = 0;
    while (written < json_len) {
        ssize_t w = write(fd, json_buf + written, json_len - written);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd);
            unlink(tmp_path);
            free(json_buf);
            free(cid);
            pthread_rwlock_unlock(&g_manifest_lock);
            return -1;
        }
        written += (size_t)w;
    }

    if (fsync(fd) < 0 || close(fd) < 0 || rename(tmp_path, path) < 0) {
        unlink(tmp_path);
        free(json_buf);
        free(cid);
        pthread_rwlock_unlock(&g_manifest_lock);
        return -1;
    }

    char owner_path[ENGINE_MAX_PATH_LEN];
    snprintf(owner_path, sizeof(owner_path), "owners/%s.owner", cid);

    if (util_mkdir_p("owners") < 0) {
        log_error("[AUTH] Failed to create owners directory");
    } else {
        if (m->auth_token && strncmp(m->auth_token, "owner-", 6) == 0) {
            FILE* f = fopen(owner_path, "w");
            if (f) {
                fprintf(f, "%s\n", m->auth_token);
                fclose(f);
            } else {
                log_error("[AUTH] Failed to create owner file: %s", owner_path);
            }
        } else {
            unlink(owner_path);
        }
    }

    free(json_buf);
    *out_cid = cid;
    pthread_rwlock_unlock(&g_manifest_lock);
    return 0;
}

int manifest_save_progress(const manifest* m, const char* upload_id) {
    if (!m || !upload_id) return -1;

    char path[ENGINE_MAX_PATH_LEN];
    snprintf(path, sizeof(path), "manifests/in-progress/%s.json", upload_id);

    FILE* f = fopen(path, "w");
    if (!f) {
        log_error("[MANIFEST] Failed to open progress file for writing: %s", path);
        return -1;
    }

    fprintf(f,
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

    for (uint32_t i = 0; i < m->chunk_count; ++i) {
        const manifest_chunk* c = &m->chunks[i];
        fprintf(f,
            "    { \"index\": %u, \"size\": %u, \"hash\": \"%s\" }%s\n",
            c->index,
            c->size,
            c->hash_str ? c->hash_str : "",
            (i + 1 < m->chunk_count) ? "," : ""
        );
    }

    fprintf(f, "  ]\n}\n");

    fclose(f);
    return 0;
}

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
    char* buf = (char*)malloc(len + 1);
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
        if (r == 0) break;
        off += (size_t)r;
    }
    close(fd);

    buf[off] = '\0';
    *out_buf = buf;
    *out_len = off;
    return 0;
}

static int manifest_extract_string(const char* json, const char* key, char* out_buf, size_t out_size) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char* p = strstr(json, pattern);
    if (!p) return -1;

    p = strchr(p, ':');
    if (!p) return -1;
    p = strchr(p, '"');
    if (!p) return -1;
    p++;

    const char* end = strchr(p, '"');
    if (!end) return -1;

    size_t len = (size_t)(end - p);
    if (len >= out_size) return -1;

    memcpy(out_buf, p, len);
    out_buf[len] = '\0';
    return 0;
}

static int manifest_extract_ull(const char* json, const char* key, unsigned long long* out_val) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char* p = strstr(json, pattern);
    if (!p) return -1;

    p = strchr(p, ':');
    if (!p) return -1;
    p++;

    while (*p == ' ' || *p == '\t') p++;

    unsigned long long v = 0;
    if (sscanf(p, "%llu", &v) != 1) {
        return -1;
    }
    *out_val = v;
    return 0;
}

static int manifest_extract_u32(const char* json, const char* key, uint32_t* out_val) {
    unsigned long long v = 0;
    if (manifest_extract_ull(json, key, &v) < 0) {
        return -1;
    }
    *out_val = (uint32_t)v;
    return 0;
}

manifest* manifest_load_from_cid(const char* cid) {
    if (!cid) {
        log_error("[MANIFEST] manifest_load_from_cid called with NULL cid");
        return NULL;
    }

    pthread_rwlock_rdlock(&g_manifest_lock);

    char path[ENGINE_MAX_PATH_LEN];
    if (manifest_make_path(cid, path, sizeof(path)) < 0) {
        log_error("[MANIFEST] failed to build path for cid=%s", cid);
        pthread_rwlock_unlock(&g_manifest_lock);
        return NULL;
    }

    char* json_buf = NULL;
    size_t json_len = 0;
    if (manifest_read_file(path, &json_buf, &json_len) < 0) {
        pthread_rwlock_unlock(&g_manifest_lock);
        return NULL;
    }

    char filename[256];
    char hash_algo[64];
    uint32_t chunk_size = 0;
    unsigned long long total_size_ull = 0;

    if (manifest_extract_string(json_buf, "filename", filename, sizeof(filename)) < 0) {
        log_error("[MANIFEST] failed to parse filename from %s", path);
        free(json_buf);
        pthread_rwlock_unlock(&g_manifest_lock);
        return NULL;
    }

    if (manifest_extract_string(json_buf, "hash_algo", hash_algo, sizeof(hash_algo)) < 0) {
        strcpy(hash_algo, "blake3");
    }

    if (manifest_extract_u32(json_buf, "chunk_size", &chunk_size) < 0) {
        log_error("[MANIFEST] failed to parse chunk_size from %s", path);
        free(json_buf);
        pthread_rwlock_unlock(&g_manifest_lock);
        return NULL;
    }

    if (manifest_extract_ull(json_buf, "total_size", &total_size_ull) < 0) {
        log_error("[MANIFEST] failed to parse total_size from %s", path);
        free(json_buf);
        pthread_rwlock_unlock(&g_manifest_lock);
        return NULL;
    }

    manifest* m = manifest_create(filename, chunk_size, hash_algo);
    if (!m) {
        log_error("[MANIFEST] manifest_create failed while loading %s", path);
        free(json_buf);
        pthread_rwlock_unlock(&g_manifest_lock);
        return NULL;
    }

    const char* p = strstr(json_buf, "\"chunks\"");
    if (!p) {
        log_error("[MANIFEST] no \"chunks\" array in %s", path);
        manifest_free(m);
        free(json_buf);
        pthread_rwlock_unlock(&g_manifest_lock);
        return NULL;
    }

    p = strchr(p, '[');
    if (!p) {
        log_error("[MANIFEST] malformed chunks array in %s", path);
        manifest_free(m);
        free(json_buf);
        pthread_rwlock_unlock(&g_manifest_lock);
        return NULL;
    }

    p++;

    while (1) {
        const char* obj_start = strchr(p, '{');
        if (!obj_start) {
            break;
        }

        const char* obj_end = strchr(obj_start, '}');
        if (!obj_end) {
            log_error("[MANIFEST] unterminated chunk object in %s", path);
            manifest_free(m);
            free(json_buf);
            pthread_rwlock_unlock(&g_manifest_lock);
            return NULL;
        }

        size_t obj_len = (size_t)(obj_end - obj_start + 1);
        if (obj_len >= 512) {
            log_error("[MANIFEST] chunk object too large in %s", path);
            manifest_free(m);
            free(json_buf);
            pthread_rwlock_unlock(&g_manifest_lock);
            return NULL;
        }

        char chunk_json[512];
        memcpy(chunk_json, obj_start, obj_len);
        chunk_json[obj_len] = '\0';

        uint32_t idx = 0;
        uint32_t size = 0;
        char hash_str[256];

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
            pthread_rwlock_unlock(&g_manifest_lock);
            return NULL;
        }

        if (manifest_add_chunk(m, idx, size, hash_str) < 0) {
            log_error("[MANIFEST] manifest_add_chunk failed while loading %s", path);
            manifest_free(m);
            free(json_buf);
            pthread_rwlock_unlock(&g_manifest_lock);
            return NULL;
        }

        p = obj_end + 1;
    }

    manifest_finalize(m, (uint64_t)total_size_ull);

    free(json_buf);

    pthread_rwlock_unlock(&g_manifest_lock);
    return m;
}

void manifest_free(manifest* m) {
    if (!m) return;

    if (m->filename) {
        free(m->filename);
        m->filename = NULL;
    }

    if (m->hash_algo) {
        free(m->hash_algo);
        m->hash_algo = NULL;
    }

    if (m->chunks) {
        for (uint32_t i = 0; i < m->chunk_count; ++i) {
            free(m->chunks[i].hash_str);
            m->chunks[i].hash_str = NULL;
        }
        free(m->chunks);
        m->chunks = NULL;
    }

    if (m->auth_token) {
        free(m->auth_token);
        m->auth_token = NULL;
    }

    free(m);
}

int manifest_delete(const char* cid) {
    if (!cid) return -1;
    manifest* m = manifest_load_from_cid(cid);
    if (!m) return -1;

    for (uint32_t i = 0; i < m->chunk_count; ++i) {
        const char* hash_str = m->chunks[i].hash_str;
        char chunk_path[ENGINE_MAX_PATH_LEN];
        if (blockstore_make_path(hash_str, chunk_path) < 0) continue;

        char ref_path[ENGINE_MAX_PATH_LEN];
        snprintf(ref_path, sizeof(ref_path), "%s.ref", chunk_path);

        FILE* f = fopen(ref_path, "r+");
        if (!f) continue;

        int count = 0;
        if (fscanf(f, "%d", &count) != 1) {
            fclose(f);
            continue;
        }
        count--;
        rewind(f);

        if (count > 0) {
            fprintf(f, "%d\n", count);
            fclose(f);
        } else {
            fclose(f);
            unlink(ref_path);
            unlink(chunk_path);
        }
    }

    char path[ENGINE_MAX_PATH_LEN];
    if (manifest_make_path(cid, path, sizeof(path)) == 0) {
        unlink(path);
    }

    manifest_free(m);
    return 0;
}

manifest* manifest_load_in_progress(const char* upload_id) {
    if (!upload_id) return NULL;

    char path[ENGINE_MAX_PATH_LEN];
    snprintf(path, sizeof(path), "manifests/in-progress/%s.json", upload_id);

    size_t len = 0;
    char* buf = NULL;
    if (manifest_read_file(path, &buf, &len) < 0 || !buf) return NULL;

    manifest* m = manifest_parse_from_json(buf);
    free(buf);
    return m;
}

manifest* manifest_parse_from_json(const char* json_buf) {
    if (!json_buf) return NULL;

    char filename[256] = {0};
    char hash_algo[64] = {0};
    uint32_t chunk_size = 0;
    unsigned long long total_size_ull = 0;

    if (manifest_extract_string(json_buf, "filename", filename, sizeof(filename)) < 0) {
        log_error("[MANIFEST] manifest_parse_from_json: failed to extract filename");
        return NULL;
    }

    if (manifest_extract_string(json_buf, "hash_algo", hash_algo, sizeof(hash_algo)) < 0) {
        strcpy(hash_algo, "blake3");
    }

    if (manifest_extract_u32(json_buf, "chunk_size", &chunk_size) < 0 ||
        manifest_extract_ull(json_buf, "total_size", &total_size_ull) < 0) {
        log_error("[MANIFEST] manifest_parse_from_json: failed to extract size fields");
        return NULL;
    }

    manifest* m = manifest_create(filename, chunk_size, hash_algo);
    if (!m) {
        log_error("[MANIFEST] manifest_parse_from_json: manifest_create failed");
        return NULL;
    }

    const char* p = strstr(json_buf, "\"chunks\"");
    if (!p || !(p = strchr(p, '['))) {
        log_error("[MANIFEST] manifest_parse_from_json: malformed chunks array");
        manifest_free(m);
        return NULL;
    }

    p++;

    while (1) {
        const char* obj_start = strchr(p, '{');
        if (!obj_start) break;

        const char* obj_end = strchr(obj_start, '}');
        if (!obj_end) {
            log_error("[MANIFEST] manifest_parse_from_json: unterminated chunk object");
            manifest_free(m);
            return NULL;
        }

        size_t obj_len = (size_t)(obj_end - obj_start + 1);
        if (obj_len >= 512) {
            log_error("[MANIFEST] manifest_parse_from_json: chunk object too large");
            manifest_free(m);
            return NULL;
        }

        char chunk_json[512];
        memcpy(chunk_json, obj_start, obj_len);
        chunk_json[obj_len] = '\0';

        uint32_t idx = 0;
        uint32_t size = 0;
        char hash_str[256];

        int matched = sscanf(chunk_json,
            " { \"index\" : %u , \"size\" : %u , \"hash\" : \"%255[^\"]\"",
            &idx, &size, hash_str);

        if (matched != 3) {
            matched = sscanf(chunk_json,
                " { \"index\": %u, \"size\": %u, \"hash\": \"%255[^\"]\"",
                &idx, &size, hash_str);
        }

        if (matched != 3) {
            log_error("[MANIFEST] manifest_parse_from_json: failed to parse chunk: %s", chunk_json);
            manifest_free(m);
            return NULL;
        }

        if (manifest_add_chunk(m, idx, size, hash_str) < 0) {
            log_error("[MANIFEST] manifest_parse_from_json: manifest_add_chunk failed");
            manifest_free(m);
            return NULL;
        }

        p = obj_end + 1;
    }

    manifest_finalize(m, (uint64_t)total_size_ull);
    return m;
}
