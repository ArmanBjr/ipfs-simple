// upload.h
#pragma once

#include <stdint.h>
#include <stddef.h>

// Forward declaration of manifest (defined in manifest.h)
struct manifest;

// Upload context for a single connection
typedef struct upload_ctx {
    char*            filename;      // original filename received from gateway
    uint64_t         total_size;    // total bytes received so far
    uint32_t         chunk_size;    // usually ENGINE_CHUNK_SIZE
    uint32_t         next_index;    // index of next chunk
    struct manifest* manifest;      // manifest being constructed
} upload_ctx;

// Create / destroy upload context
upload_ctx* upload_ctx_create(void);
void        upload_ctx_destroy(upload_ctx* ctx);

// Handle UPLOAD_START: payload contains filename
int upload_handle_start(upload_ctx* ctx, const uint8_t* payload, uint32_t len);

// Handle UPLOAD_CHUNK: raw file data chunk
int upload_handle_chunk(upload_ctx* ctx, const uint8_t* data, uint32_t len);

// Handle UPLOAD_FINISH: finalize manifest and return CID
// out_cid is malloc'ed inside and must be freed by caller
int upload_handle_finish(upload_ctx* ctx, char** out_cid);
