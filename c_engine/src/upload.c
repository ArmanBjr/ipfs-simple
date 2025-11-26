// src/upload.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

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

    // Initialize basic fields
    ctx->filename         = NULL;
    ctx->total_size       = 0;
    ctx->chunk_size       = ENGINE_CHUNK_SIZE;
    ctx->next_chunk_index = 0;
    ctx->manifest         = NULL;

    // Initialize streaming buffer
    ctx->buffer_len = 0;
    ctx->buffer = (uint8_t*)malloc(ctx->chunk_size);
    if (!ctx->buffer) {
        fprintf(stderr, "[UPLOAD] ERROR: failed to allocate upload buffer\n");
        free(ctx);
        return NULL;
    }

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

    // Free filename
    if (ctx->filename) {
        free(ctx->filename);
        ctx->filename = NULL;
    }

    // Free manifest (if allocated)
    if (ctx->manifest) {
        manifest_free(ctx->manifest);
        ctx->manifest = NULL;
    }

    // Free stream buffer
    if (ctx->buffer) {
        free(ctx->buffer);
        ctx->buffer = NULL;
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

    // Payload format:
    // [0..7]   : total_size (uint64_t, big-endian, as declared by client)
    // [8..len) : filename bytes (not null-terminated)
    if (!payload || len < 8) {
        fprintf(stderr, "[UPLOAD] ERROR: UPLOAD_START payload too short (len=%u)\n", len);
        return -1;
    }

    // Parse total_size from big-endian bytes
    uint64_t declared_total_size = 0;
    for (int i = 0; i < 8; i++) {
        declared_total_size = (declared_total_size << 8) | (uint64_t)payload[i];
    }

    const uint8_t* fname_bytes = payload + 8;
    uint32_t fname_len = len - 8;

    // Parse filename (if any)
    if (fname_len > 0) {
        ctx->filename = (char*)malloc((size_t)fname_len + 1);
        if (!ctx->filename) {
            fprintf(stderr, "[UPLOAD] ERROR: failed to allocate filename (len=%u)\n", fname_len);
            return -1;
        }
        memcpy(ctx->filename, fname_bytes, fname_len);
        ctx->filename[fname_len] = '\0';
    } else {
        // Fallback if no filename is provided.
        ctx->filename = strdup("unnamed");
        if (!ctx->filename) {
            fprintf(stderr, "[UPLOAD] ERROR: failed to allocate default filename\n");
            return -1;
        }
    }

    // Initialize upload state for a fresh upload.
    // total_size will be accumulated from processed chunks.
    ctx->total_size       = 0;
    ctx->next_chunk_index = 0;
    ctx->buffer_len       = 0;  // streaming buffer is empty at start

    // Create manifest for this upload (hash_algo = "blake3" for now).
    ctx->manifest = manifest_create(ctx->filename, ctx->chunk_size, "blake3");
    if (!ctx->manifest) {
        fprintf(stderr, "[UPLOAD] ERROR: manifest_create failed\n");
        return -1;
    }

    fprintf(stderr,
            "[UPLOAD] start: filename=\"%s\", declared_total_size=%llu, chunk_size=%u (manifest created)\n",
            ctx->filename,
            (unsigned long long)declared_total_size,
            ctx->chunk_size);

    return 0;
}


// Process a single, full chunk of data.
// - compute content hash
// - convert to multihash (base32 string)
// - store block in the blockstore
// - append chunk entry to the manifest
static int upload_process_full_chunk(upload_ctx* ctx, const uint8_t* data, uint32_t len) {
    if (!ctx || !data || len == 0) {
        fprintf(stderr, "[UPLOAD] ERROR: upload_process_full_chunk: invalid arguments\n");
        return -1;
    }
    if (!ctx->manifest) {
        fprintf(stderr, "[UPLOAD] ERROR: upload_process_full_chunk: manifest is NULL\n");
        return -1;
    }

    hash_algo_t algo = hash_algo_from_env();
    hash_result_t h;
    memset(&h, 0, sizeof(h));

    if (hash_compute(algo, data, (size_t)len, &h) < 0) {
        fprintf(stderr, "[UPLOAD] ERROR: failed to compute chunk hash\n");
        return -1;
    }

    char* mh_str = NULL;
    if (hash_to_multihash_b32(&h, &mh_str) < 0 || !mh_str) {
        fprintf(stderr, "[UPLOAD] ERROR: failed to convert hash to multihash base32\n");
        hash_result_free(&h);
        return -1;
    }

    // Store the block in the blockstore (deduplicated inside blockstore_put).
    if (blockstore_put(mh_str, data, (size_t)len) < 0) {
        fprintf(stderr, "[UPLOAD] ERROR: blockstore_put failed for chunk %u\n", ctx->next_chunk_index);
        hash_result_free(&h);
        free(mh_str);
        return -1;
    }

    // Record this chunk in the manifest.
    if (manifest_add_chunk(ctx->manifest, ctx->next_chunk_index, len, mh_str) < 0) {
        fprintf(stderr, "[UPLOAD] ERROR: manifest_add_chunk failed for chunk %u\n", ctx->next_chunk_index);
        hash_result_free(&h);
        free(mh_str);
        return -1;
    }

    // Update counters: we still keep total_size as "bytes actually chunked".
    ctx->total_size      += (uint64_t)len;
    ctx->next_chunk_index++;

    hash_result_free(&h);
    free(mh_str);

    fprintf(stderr,
        "[UPLOAD] processed full chunk index=%u, size=%u (total_size=%" PRIu64 ")\n",
        ctx->next_chunk_index - 1, len, ctx->total_size);

    return 0;
}

// High-level stream handler:
// Accepts arbitrary-sized pieces of the upload stream, buffering until a full chunk is ready.
// Once the buffer accumulates ctx->chunk_size bytes, it is processed as a chunk.
int upload_handle_stream_data(upload_ctx* ctx, const uint8_t* data, uint32_t len) {
    if (!ctx || !data) {
        fprintf(stderr, "[UPLOAD] ERROR: upload_handle_stream_data: invalid arguments\n");
        return -1;
    }
    if (!ctx->manifest) {
        fprintf(stderr, "[UPLOAD] ERROR: upload_handle_stream_data: manifest is NULL (UPLOAD_START not done?)\n");
        return -1;
    }
    if (len == 0) {
        // No data to process; not an error.
        return 0;
    }

    if (!ctx->buffer || ctx->chunk_size == 0) {
        fprintf(stderr, "[UPLOAD] ERROR: upload_handle_stream_data: buffer or chunk_size not initialized\n");
        return -1;
    }

    const uint8_t* p = data;
    uint32_t remaining = len;

    while (remaining > 0) {
        uint32_t free_space = ctx->chunk_size - ctx->buffer_len;
        uint32_t to_copy = (remaining < free_space) ? remaining : free_space;

        // Copy to staging buffer
        memcpy(ctx->buffer + ctx->buffer_len, p, to_copy);
        ctx->buffer_len += to_copy;
        p += to_copy;
        remaining -= to_copy;

        // Process full chunk if buffer is full
        if (ctx->buffer_len == ctx->chunk_size) {
            if (upload_process_full_chunk(ctx, ctx->buffer, ctx->chunk_size) < 0) {
                // Error already logged; reset buffer_len and return error
                ctx->buffer_len = 0;
                return -1;
            }
            // Reset buffer for next chunk
            ctx->buffer_len = 0;
        }
    }

    return 0;
}

// Public entry point used by c_engine.c for each OP_UPLOAD_CHUNK frame.
// This now treats the incoming frame as arbitrary stream data and lets
// upload_handle_stream_data() take care of buffering and chunking.
int upload_handle_chunk(upload_ctx* ctx, const uint8_t* data, uint32_t len) {
    if (!ctx || !data) {
        fprintf(stderr, "[UPLOAD] ERROR: upload_handle_chunk: invalid arguments\n");
        return -1;
    }
    if (!ctx->manifest) {
        fprintf(stderr, "[UPLOAD] ERROR: upload_handle_chunk: manifest is NULL (UPLOAD_START not done?)\n");
        return -1;
    }

    return upload_handle_stream_data(ctx, data, len);
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

    // Flush the last partial chunk if there is any data left in the buffer.
    if (ctx->buffer && ctx->buffer_len > 0) {
        fprintf(stderr,
                "[UPLOAD] finish: flushing final partial chunk (size=%u)\n",
                ctx->buffer_len);

        if (upload_process_full_chunk(ctx,
                                      ctx->buffer,
                                      ctx->buffer_len) < 0) {
            fprintf(stderr,
                    "[UPLOAD] ERROR: failed to process final partial chunk\n");
            return -1;
        }

        // Buffer is now fully consumed.
        ctx->buffer_len = 0;
    }

    // At this point, ctx->total_size is the sum of all processed chunk sizes.
    manifest_finalize(ctx->manifest, ctx->total_size);

    // Save manifest to disk and obtain CID.
    char* cid = NULL;
    if (manifest_save_and_get_cid(ctx->manifest, &cid) < 0 || !cid) {
        fprintf(stderr, "[UPLOAD] ERROR: manifest_save_and_get_cid failed\n");
        return -1;
    }

    // Free manifest structure in memory.
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
