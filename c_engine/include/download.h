// include/download.h
#pragma once

#include <stdint.h>
#include <stddef.h>

struct manifest;

// Per-connection download state.
// A new download_ctx is created at DOWNLOAD_START and destroyed at the end.
typedef struct download_ctx {
    // CID of the file being downloaded.
    // Allocated with malloc() and freed in download_ctx_destroy().
    char* cid;

    // Manifest loaded from disk (NULL in this phase).
    struct manifest* manifest;

    // Index of the next chunk to send back to the client.
    uint32_t current_index;

} download_ctx;

// Allocate and initialize a new download_ctx.
// Returns NULL on failure.
download_ctx* download_ctx_create(void);

// Free all resources associated with a download_ctx.
// - frees cid
// - frees manifest (later phases)
// - frees ctx itself
void download_ctx_destroy(download_ctx* ctx);

// Initialize download_ctx for a given CID.
// In this phase:
//   - save a malloc()'d copy of the CID
//   - set current_index = 0
//   - manifest loading will be added in later phases
// Return 0 on success, <0 on error.
int download_init(download_ctx* ctx, const char* cid);

// Retrieve the next chunk for DOWNLOAD_CHUNK.
//
// In this phase:
//   - no real download logic exists
//   - always returns EOF (0)
//   - *out_data = NULL and *out_len = 0
//
// Expected return codes:
//   1 → a valid chunk is returned (future phases)
//   0 → EOF (no more chunks)
//  <0 → error
int download_next_chunk(download_ctx* ctx, uint8_t** out_data, uint32_t* out_len);
