// src/upload.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine_config.h"
#include "upload.h"

#include "manifest.h"
#include "hash.h"
#include "blockstore.h"

upload_ctx* upload_ctx_create(void) {
    upload_ctx* ctx = (upload_ctx*)calloc(1, sizeof(upload_ctx));
    if (!ctx) {
        fprintf(stderr, "[UPLOAD] ERROR: failed to allocate upload_ctx\n");
        return NULL;
    }

    // Explicit initialization for clarity in Phase 6
    ctx->filename        = NULL;
    ctx->total_size      = 0;
    ctx->chunk_size      = ENGINE_CHUNK_SIZE;
    ctx->next_chunk_index = 0;
    ctx->manifest        = NULL;

    fprintf(stderr,
            "[UPLOAD] upload_ctx created (chunk_size=%u)\n",
            ctx->chunk_size);
    return ctx;
}

// Destroy an upload context and free all associated resources.
void upload_ctx_destroy(upload_ctx* ctx) {
    if (!ctx) {
        return;
    }

    fprintf(stderr, "[UPLOAD] upload_ctx_destroy called\n");

    if (ctx->filename) {
        free(ctx->filename);
        ctx->filename = NULL;
    }

    // manifest is a forward-declared struct; in later phases we will
    // properly free it here once manifest.c is implemented.
    if (ctx->manifest) {
        manifest_free(ctx->manifest);
        ctx->manifest = NULL;
    }

    

    free(ctx);
}

int upload_handle_start(upload_ctx* ctx, const uint8_t* payload, uint32_t len) {
    if (!ctx) {
        fprintf(stderr, "[UPLOAD] ERROR: upload_handle_start called with NULL ctx\n");
        return -1;
    }

    // Prevent multiple START on the same context.
    if (ctx->filename != NULL || ctx->manifest != NULL) {
        fprintf(stderr, "[UPLOAD] ERROR: UPLOAD_START called twice on the same context\n");
        return -1;
    }

    // Interpret the entire payload as the filename bytes (not null-terminated).
    if (payload && len > 0) {
        ctx->filename = (char*)malloc((size_t)len + 1);
        if (!ctx->filename) {
            fprintf(stderr, "[UPLOAD] ERROR: failed to allocate filename\n");
            return -1;
        }
        memcpy(ctx->filename, payload, len);
        ctx->filename[len] = '\0';
    } else {
        // Fallback if no filename is provided.
        ctx->filename = strdup("unnamed");
        if (!ctx->filename) {
            fprintf(stderr, "[UPLOAD] ERROR: failed to allocate default filename\n");
            return -1;
        }
    }

    // Reset counters for a fresh upload.
    ctx->total_size       = 0;
    ctx->next_chunk_index = 0;
    // ctx->chunk_size was set in upload_ctx_create (ENGINE_CHUNK_SIZE).

    // Create manifest for this upload (hash_algo = "blake3" for now).
    ctx->manifest = manifest_create(ctx->filename, ctx->chunk_size, "blake3");
    if (!ctx->manifest) {
        fprintf(stderr,
                "[UPLOAD] ERROR: manifest_create failed for filename=\"%s\"\n",
                ctx->filename);
        free(ctx->filename);
        ctx->filename = NULL;
        return -1;
    }

    fprintf(stderr,
            "[UPLOAD] start: filename=\"%s\", chunk_size=%u (manifest created)\n",
            ctx->filename,
            ctx->chunk_size);

    // Later we will parse total_size, chunk_size, hash_algo, etc. from payload.
    return 0;
}


int upload_handle_chunk(upload_ctx* ctx, const uint8_t* data, uint32_t len) {
    if (!ctx) {
        fprintf(stderr, "[UPLOAD] ERROR: upload_handle_chunk called with NULL ctx\n");
        return -1;
    }

    if (!data && len > 0) {
        fprintf(stderr, "[UPLOAD] ERROR: upload_handle_chunk called with NULL data (len=%u)\n", len);
        return -1;
    }

    // UPLOAD_START has to be called before UPLO
    if (!ctx->filename) {
        fprintf(stderr, "[UPLOAD] ERROR: UPLOAD_CHUNK received before UPLOAD_START\n");
        return -1;
    }

    if (!ctx->manifest) {
        fprintf(stderr, "[UPLOAD] ERROR: upload_handle_chunk: ctx->manifest is NULL\n");
        return -1;
    }

    // 1) Compute hash of this chunk
    hash_algo_t algo = hash_algo_from_env();
    hash_result_t h;
    memset(&h, 0, sizeof(h));

    if (hash_compute(algo, data, len, &h) < 0) {
        fprintf(stderr, "[UPLOAD] ERROR: hash_compute failed (len=%u)\n", len);
        return -1;
    }

    // 2) Convert hash result to multihash "base32" 
    char* hash_str = NULL;
    if (hash_to_multihash_b32(&h, &hash_str) < 0 || !hash_str) {
        fprintf(stderr, "[UPLOAD] ERROR: hash_to_multihash_b32 failed\n");
        hash_result_free(&h);
        return -1;
    }

    // 3) Store block in blockstore
    if (blockstore_put(hash_str, data, len) < 0) {
        fprintf(stderr,
                "[UPLOAD] ERROR: blockstore_put failed for hash=%s (len=%u)\n",
                hash_str, len);
        hash_result_free(&h);
        free(hash_str);
        return -1;
    }

    // 4) Add this chunk to manifest
    if (manifest_add_chunk(ctx->manifest,
                           ctx->next_chunk_index,
                           len,
                           hash_str) < 0) {
        fprintf(stderr,
                "[UPLOAD] ERROR: manifest_add_chunk failed (index=%u, len=%u)\n",
                ctx->next_chunk_index, len);
        hash_result_free(&h);
        free(hash_str);
        return -1;
    }

    // 5) Update counters
    ctx->total_size       += (uint64_t)len;
    ctx->next_chunk_index += 1;

    // 6) Cleanup temporary hash structures
    hash_result_free(&h);
    free(hash_str);

    fprintf(stderr,
            "[UPLOAD] chunk stored: index=%u, len=%u, total_size=%llu\n",
            ctx->next_chunk_index - 1,
            len,
            (unsigned long long)ctx->total_size);

    return 0;
}

int upload_handle_finish(upload_ctx* ctx, char** out_cid) {
    if (!ctx) {
        fprintf(stderr, "[UPLOAD] ERROR: upload_handle_finish called with NULL ctx\n");
        return -1;
    }
    if (!out_cid) {
        fprintf(stderr, "[UPLOAD] ERROR: upload_handle_finish called with NULL out_cid\n");
        return -1;
    }

    *out_cid = NULL;

    if (!ctx->filename) {
        fprintf(stderr, "[UPLOAD] ERROR: upload_handle_finish called before UPLOAD_START\n");
        return -1;
    }

    if (!ctx->manifest) {
        fprintf(stderr, "[UPLOAD] ERROR: upload_handle_finish: ctx->manifest is NULL\n");
        return -1;
    }

    // 1) Finalize manifest with total file size
    manifest_finalize(ctx->manifest, ctx->total_size);

    // 2) Save manifest to disk and obtain CID
    char* cid = NULL;
    if (manifest_save_and_get_cid(ctx->manifest, &cid) < 0 || !cid) {
        fprintf(stderr, "[UPLOAD] ERROR: manifest_save_and_get_cid failed\n");
        return -1;
    }

    // 3) Free manifest structure in memory
    manifest_free(ctx->manifest);
    ctx->manifest = NULL;

    *out_cid = cid;

    fprintf(stderr,
            "[UPLOAD] finish: filename=\"%s\", total_size=%llu, chunks=%u, cid=%s\n",
            ctx->filename ? ctx->filename : "(null)",
            (unsigned long long)ctx->total_size,
            ctx->next_chunk_index,
            cid);

    return 0;
}
