// src/upload.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine_config.h"
#include "upload.h"

// Create a new upload context for a single connection / upload.
upload_ctx* upload_ctx_create(void) {
    upload_ctx* ctx = (upload_ctx*)calloc(1, sizeof(upload_ctx));
    if (!ctx) {
        fprintf(stderr, "[UPLOAD] ERROR: failed to allocate upload_ctx\n");
        return NULL;
    }

    // All fields are zero-initialized by calloc.
    // We set the default chunk size here; it can be overridden later if needed.
    ctx->chunk_size = ENGINE_CHUNK_SIZE;

    fprintf(stderr, "[UPLOAD] upload_ctx created (chunk_size=%u)\n", ctx->chunk_size);
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
        // TODO: call manifest_free(ctx->manifest) in later phases.
        ctx->manifest = NULL;
    }

    free(ctx);
}

// Handle the UPLOAD_START frame.
// payload/len currently expected to contain the filename (and possibly more metadata later).
int upload_handle_start(upload_ctx* ctx, const uint8_t* payload, uint32_t len) {
    if (!ctx) {
        fprintf(stderr, "[UPLOAD] ERROR: upload_handle_start called with NULL ctx\n");
        return -1;
    }

    // Defensive check: prevent multiple START calls on the same ctx.
    if (ctx->filename != NULL) {
        fprintf(stderr, "[UPLOAD] ERROR: UPLOAD_START called twice on the same context\n");
        return -1;
    }

    // For now, interpret the entire payload as the filename bytes (not null-terminated).
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

    ctx->total_size       = 0;
    ctx->next_chunk_index = 0;
    // chunk_size is already set in create; can be overridden when we parse metadata.

    fprintf(stderr,
            "[UPLOAD] start: filename=\"%s\", chunk_size=%u\n",
            ctx->filename,
            ctx->chunk_size);

    // Later we will parse total_size, chunk_size, hash_algo, etc. from payload.
    return 0;
}

// Handle each UPLOAD_CHUNK frame.
// For now we only update state and log; no real storage yet.
int upload_handle_chunk(upload_ctx* ctx, const uint8_t* data, uint32_t len) {
    if (!ctx) {
        fprintf(stderr, "[UPLOAD] ERROR: upload_handle_chunk called with NULL ctx\n");
        return -1;
    }

    // If start was never called, filename will be NULL.
    if (!ctx->filename) {
        fprintf(stderr, "[UPLOAD] ERROR: UPLOAD_CHUNK received before UPLOAD_START\n");
        return -1;
    }

    // In later phases, we will hash and store this chunk in blockstore.
    ctx->total_size       += (uint64_t)len;
    ctx->next_chunk_index += 1;

    fprintf(stderr,
            "[UPLOAD] chunk: index=%u, len=%u, total_size=%llu\n",
            ctx->next_chunk_index - 1,
            len,
            (unsigned long long)ctx->total_size);

    (void)data; // data is unused in this stub phase.
    return 0;
}

// Handle UPLOAD_FINISH.
// For now we generate a placeholder CID string and log some info.
int upload_handle_finish(upload_ctx* ctx, char** out_cid) {
    if (!ctx) {
        fprintf(stderr, "[UPLOAD] ERROR: upload_handle_finish called with NULL ctx\n");
        return -1;
    }
    if (!out_cid) {
        fprintf(stderr, "[UPLOAD] ERROR: upload_handle_finish called with NULL out_cid\n");
        return -1;
    }

    // In later phases, we will:
    //  - finalize the manifest,
    //  - hash the manifest JSON to compute a real CID,
    //  - store the manifest under manifests/<cid>.json.
    //
    // For now we just return a static placeholder CID.
    const char* placeholder = "CID-PLACEHOLDER";
    char* cid_copy = strdup(placeholder);
    if (!cid_copy) {
        fprintf(stderr, "[UPLOAD] ERROR: failed to allocate CID string\n");
        return -1;
    }

    *out_cid = cid_copy;

    fprintf(stderr,
            "[UPLOAD] finish: filename=\"%s\", total_size=%llu, chunks=%u, cid=%s\n",
            ctx->filename ? ctx->filename : "(null)",
            (unsigned long long)ctx->total_size,
            ctx->next_chunk_index,
            cid_copy);

    return 0;
}
