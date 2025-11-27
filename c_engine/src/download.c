// src/download.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "download.h"
#include "manifest.h"
#include "blockstore.h"
#include "hash.h"

// Create a new download context for a single connection / download.
download_ctx* download_ctx_create(void) {
    download_ctx* ctx = (download_ctx*)calloc(1, sizeof(download_ctx));
    if (!ctx) {
        fprintf(stderr, "[DOWNLOAD] ERROR: failed to allocate download_ctx\n");
        return NULL;
    }

    // All fields are zero-initialized by calloc.
    fprintf(stderr, "[DOWNLOAD] download_ctx created\n");
    return ctx;
}

// Destroy a download context and free all associated resources.
void download_ctx_destroy(download_ctx* ctx) {
    if (!ctx) {
        return;
    }

    fprintf(stderr, "[DOWNLOAD] download_ctx_destroy called\n");

    if (ctx->cid) {
        free(ctx->cid);
        ctx->cid = NULL;
    }

    if (ctx->manifest) {
        manifest_free(ctx->manifest);
        ctx->manifest = NULL;
    }

    if (ctx->cur_block) {
        free(ctx->cur_block);
        ctx->cur_block = NULL;
    }

    ctx->cur_block_len = 0;
    ctx->cur_block_pos = 0;

    free(ctx);
}

// Initialize a download context with a given CID:
// - store CID
// - load manifest from disk
// - reset streaming state
int download_init(download_ctx* ctx, const char* cid) {
    if (!ctx) {
        fprintf(stderr, "[DOWNLOAD] ERROR: download_init called with NULL ctx\n");
        return -1;
    }
    if (!cid) {
        fprintf(stderr, "[DOWNLOAD] ERROR: download_init called with NULL cid\n");
        return -1;
    }

    // Free previous cid if this context is reused
    if (ctx->cid) {
        free(ctx->cid);
        ctx->cid = NULL;
    }

    ctx->cid = strdup(cid);
    if (!ctx->cid) {
        fprintf(stderr, "[DOWNLOAD] ERROR: failed to allocate cid copy\n");
        return -1;
    }

    // If there was an old manifest, free it first
    if (ctx->manifest) {
        manifest_free(ctx->manifest);
        ctx->manifest = NULL;
    }

    // Load manifest from disk using the CID
    ctx->manifest = manifest_load_from_cid(cid);
    if (!ctx->manifest) {
        fprintf(stderr,
                "[DOWNLOAD] ERROR: failed to load manifest for cid=%s\n",
                cid);

        return -1;
    }

    // Start from the first manifest chunk
    ctx->current_index = 0;

    // Reset streaming state
    if (ctx->cur_block) {
        free(ctx->cur_block);
        ctx->cur_block = NULL;
    }
    ctx->cur_block_len = 0;
    ctx->cur_block_pos = 0;

    fprintf(stderr,
            "[DOWNLOAD] init: cid=%s, chunks=%u, total_size=%llu\n",
            ctx->cid,
            ctx->manifest->chunk_count,
            (unsigned long long)ctx->manifest->total_size);

    return 0;
}

// INTERNAL: load a full manifest chunk (block) into memory.
//
// On success:
//   - allocates *out_data with malloc()
//   - sets *out_len
//   - advances ctx->current_index
//   - returns 1
// On EOF (no more chunks):
//   - returns 0
// On error:
//   - returns -1
static int download_load_full_block(download_ctx* ctx,
                                    uint8_t** out_data,
                                    uint32_t* out_len) {
    if (!ctx || !out_data || !out_len) {
        fprintf(stderr,
                "[DOWNLOAD] ERROR: download_load_full_block: invalid args\n");
        return -1;
    }

    *out_data = NULL;
    *out_len  = 0;

    if (!ctx->manifest) {
        fprintf(stderr,
                "[DOWNLOAD] ERROR: download_load_full_block: no manifest\n");
        return -1;
    }

    // EOF: all manifest chunks consumed
    if (ctx->current_index >= ctx->manifest->chunk_count) {
        fprintf(stderr,
                "[DOWNLOAD] load_full_block: cid=%s, index=%u -> EOF\n",
                ctx->cid ? ctx->cid : "(null)",
                ctx->current_index);
        return 0;
    }

    uint32_t idx = ctx->current_index;
    const manifest_chunk* c = &ctx->manifest->chunks[idx];

    // 1) Load block data from blockstore
    uint8_t* buf = NULL;
    size_t   real_len = 0;
    if (blockstore_get(c->hash_str, &buf, &real_len) < 0) {
        fprintf(stderr,
                "[DOWNLOAD] ERROR: blockstore_get failed for hash=%s (index=%u)\n",
                c->hash_str ? c->hash_str : "(null)",
                c->index);
        return -1;
    }

    // 2) Sanity check: size from manifest vs actual file size
    if (real_len != c->size) {
        fprintf(stderr,
                "[DOWNLOAD] WARNING: size mismatch for chunk index=%u: manifest=%u, file=%zu\n",
                c->index, c->size, real_len);
        // We continue, but this is suspicious.
    }

    // 3) Optional: verify hash (using current hash implementation)
    hash_result_t h;
    memset(&h, 0, sizeof(h));
    char* mh_str = NULL;

    if (hash_compute(hash_algo_from_env(), buf, real_len, &h) < 0) {
        fprintf(stderr,
                "[DOWNLOAD] ERROR: hash_compute failed for chunk index=%u\n",
                c->index);
        free(buf);
        return -1;
    }

    if (hash_to_multihash_b32(&h, &mh_str) < 0) {
        fprintf(stderr,
                "[DOWNLOAD] ERROR: hash_to_multihash_b32 failed for chunk index=%u\n",
                c->index);
        hash_result_free(&h);
        free(buf);
        return -1;
    }

    // Compare stored hash string with recomputed one
    if (!c->hash_str || strcmp(c->hash_str, mh_str) != 0) {
        fprintf(stderr,
                "[DOWNLOAD] ERROR: hash mismatch for chunk index=%u\n"
                "         manifest:   %s\n"
                "         recomputed: %s\n",
                c->index,
                c->hash_str ? c->hash_str : "(null)",
                mh_str ? mh_str : "(null)");
        hash_result_free(&h);
        free(mh_str);
        free(buf);
        return -1;
    }

    hash_result_free(&h);
    free(mh_str);

    *out_data = buf;
    *out_len  = (uint32_t)real_len;

    fprintf(stderr,
            "[DOWNLOAD] load_full_block: cid=%s, index=%u, len=%u (hash OK)\n",
            ctx->cid ? ctx->cid : "(null)",
            idx,
            *out_len);

    ctx->current_index++;
    return 1;
}

// Stream-oriented API:
// Fill 'out_buf' with up to 'max_len' bytes of file data.
// Internally manages cur_block / cur_block_pos to slice manifest blocks
// into arbitrary-sized frames.
//
// Returns:
//   1 -> some data written, *out_len > 0
//   0 -> EOF (no more data)
//  -1 -> error
int download_stream_next(download_ctx* ctx,
                         uint8_t* out_buf,
                         uint32_t max_len,
                         uint32_t* out_len) {
    if (!ctx || !out_buf || !out_len) {
        fprintf(stderr,
                "[DOWNLOAD] ERROR: download_stream_next: invalid args\n");
        return -1;
    }

    *out_len = 0;

    if (!ctx->manifest) {
        fprintf(stderr,
                "[DOWNLOAD] ERROR: download_stream_next: no manifest\n");
        return -1;
    }

    // Ensure we have some data in cur_block.
    // If cur_block is exhausted (or not allocated yet), load the next block.
    while (ctx->cur_block_pos >= ctx->cur_block_len) {
        // Free previous block if any
        if (ctx->cur_block) {
            free(ctx->cur_block);
            ctx->cur_block = NULL;
        }
        ctx->cur_block_len = 0;
        ctx->cur_block_pos = 0;

        uint8_t* new_block = NULL;
        uint32_t new_len   = 0;
        int rc = download_load_full_block(ctx, &new_block, &new_len);
        if (rc < 0) {
            // Error already logged
            return -1;
        } else if (rc == 0) {
            // EOF: no more blocks
            return 0;
        }

        ctx->cur_block     = new_block;
        ctx->cur_block_len = new_len;
        ctx->cur_block_pos = 0;
    }

    // Now we have some bytes available in cur_block.
    uint32_t remaining = ctx->cur_block_len - ctx->cur_block_pos;
    uint32_t to_copy   = (remaining < max_len) ? remaining : max_len;

    memcpy(out_buf, ctx->cur_block + ctx->cur_block_pos, to_copy);
    ctx->cur_block_pos += to_copy;
    *out_len = to_copy;

    return 1;
}
