// include/upload.h
#pragma once

#include <stdint.h>
#include <stddef.h>

// Forward declaration for the manifest structure.
// The real definition will appear in later phases.
struct manifest;

// Per-connection upload state.
// A new upload_ctx is created at UPLOAD_START and destroyed after UPLOAD_FINISH.
typedef struct upload_ctx {
    // Filename extracted from the UPLOAD_START payload.
    // Allocated with malloc() and freed in upload_ctx_destroy().
    char* filename;

    // Total number of bytes received through CHUNK frames so far.
    uint64_t total_size;

    // Chunk size used for this upload. Usually ENGINE_CHUNK_SIZE.
    uint32_t chunk_size;

    // Index of the next chunk expected. Starts at 0 and increments with each CHUNK.
    uint32_t next_chunk_index;

    // Pointer to the manifest object (NULL during this phase).
    struct manifest* manifest;

} upload_ctx;

// Allocate and initialize a new upload_ctx.
// All fields are set to safe starting values.
// Returns NULL on failure.
upload_ctx* upload_ctx_create(void);

// Free all resources associated with an upload_ctx.
// - frees ctx->filename
// - frees ctx->manifest (in later phases)
// - frees ctx itself
void upload_ctx_destroy(upload_ctx* ctx);

// Handle the UPLOAD_START frame.
//
// payload/len: raw bytes of the frame payload.
// In this phase:
//   - interpret payload as a filename (not null-terminated).
//   - set chunk_size, reset counters, and log state.
// Return 0 on success, <0 on error.
int upload_handle_start(upload_ctx* ctx, const uint8_t* payload, uint32_t len);

// Handle each UPLOAD_CHUNK frame.
//
// data/len: chunk bytes.
// In this phase:
//   - increase total_size and next_chunk_index
//   - no storage or hashing yet (stub mode)
// Return 0 on success, <0 on error.
int upload_handle_chunk(upload_ctx* ctx, const uint8_t* data, uint32_t len);

// Handle UPLOAD_FINISH.
//
// out_cid: on success, *out_cid is set to a malloc()'d string.
// In this phase:
//   - generate a placeholder CID ("CID-PLACEHOLDER")
//   - log final stats
// Return 0 on success, <0 on error.
// Note: upload_ctx_destroy() is called later by the caller (c_engine.c).
int upload_handle_finish(upload_ctx* ctx, char** out_cid);
