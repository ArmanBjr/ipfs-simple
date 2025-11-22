// src/download.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "download.h"

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
        // TODO: call manifest_free(ctx->manifest) in later phases.
        ctx->manifest = NULL;
    }

    free(ctx);
}

// Initialize download context for a given CID.
// In later phases we will also load the manifest here.
int download_init(download_ctx* ctx, const char* cid) {
    if (!ctx) {
        fprintf(stderr, "[DOWNLOAD] ERROR: download_init called with NULL ctx\n");
        return -1;
    }
    if (!cid) {
        fprintf(stderr, "[DOWNLOAD] ERROR: download_init called with NULL cid\n");
        return -1;
    }

    if (ctx->cid != NULL) {
        fprintf(stderr, "[DOWNLOAD] WARNING: download_init called on an already initialized ctx\n");
        free(ctx->cid);
        ctx->cid = NULL;
    }

    ctx->cid = strdup(cid);
    if (!ctx->cid) {
        fprintf(stderr, "[DOWNLOAD] ERROR: failed to allocate cid copy\n");
        return -1;
    }

    ctx->current_index = 0;
    // ctx->manifest will be loaded in a later phase.

    fprintf(stderr, "[DOWNLOAD] init: cid=%s\n", ctx->cid);
    return 0;
}

// Retrieve the next chunk to send to the client.
//
// Stub behavior for this phase:
//  - On the first call (current_index == 0), return a single test chunk "DUMMY".
//  - On subsequent calls, report EOF (no more chunks).
//
// Return codes:
//   1 -> a valid chunk is returned
//   0 -> EOF (no more chunks)
//  <0 -> error
int download_next_chunk(download_ctx* ctx, uint8_t** out_data, uint32_t* out_len) {
    if (!ctx) {
        fprintf(stderr, "[DOWNLOAD] ERROR: download_next_chunk called with NULL ctx\n");
        return -1;
    }

    if (!out_data || !out_len) {
        fprintf(stderr, "[DOWNLOAD] ERROR: download_next_chunk called with NULL out_data/out_len\n");
        return -1;
    }

    // First call: return a single test chunk.
    if (ctx->current_index == 0) {
        const char* test_str = "DUMMY";
        const uint32_t len = 5; // length of "DUMMY" without null terminator

        uint8_t* buf = (uint8_t*)malloc(len);
        if (!buf) {
            fprintf(stderr, "[DOWNLOAD] ERROR: failed to allocate test chunk buffer\n");
            return -1;
        }

        memcpy(buf, test_str, len);

        *out_data = buf;
        *out_len  = len;

        fprintf(stderr,
                "[DOWNLOAD] next_chunk: cid=%s, index=%u (stub: sending test chunk \"%s\")\n",
                ctx->cid ? ctx->cid : "(null)",
                ctx->current_index,
                test_str);

        ctx->current_index += 1;
        return 1; // one valid chunk
    }

    // Subsequent calls: EOF.
    *out_data = NULL;
    *out_len  = 0;

    fprintf(stderr,
            "[DOWNLOAD] next_chunk: cid=%s, index=%u (stub: EOF)\n",
            ctx->cid ? ctx->cid : "(null)",
            ctx->current_index);

    return 0; // EOF, no more chunks
}
