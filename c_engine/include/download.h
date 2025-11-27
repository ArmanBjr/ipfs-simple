// include/download.h
#pragma once

#include <stdint.h>
#include <stddef.h>

// Forward declaration for the manifest structure.
struct manifest;

// Per-connection download state.
// A new download_ctx is created at DOWNLOAD_START and destroyed when
// the download completes or the connection closes.
typedef struct download_ctx {
    // CID of the file being downloaded (malloc'ed string).
    char* cid;

    // Loaded manifest for this CID.
    struct manifest* manifest;

    // Index of the next manifest chunk (block) that should be used.
    // This walks over manifest->chunks[].
    uint32_t current_index;

    // Streaming state:
    // We keep the currently loaded block (one manifest chunk) here
    // and slice it into smaller pieces for each OP_DOWNLOAD_CHUNK frame.
    uint8_t* cur_block;      // malloc'ed buffer holding current block data
    uint32_t cur_block_len;  // total bytes in cur_block
    uint32_t cur_block_pos;  // how many bytes already sent from cur_block
} download_ctx;

// Allocate and initialize a new download_ctx.
// Returns NULL on failure.
download_ctx* download_ctx_create(void);

// Free all resources associated with a download_ctx.
void download_ctx_destroy(download_ctx* ctx);

// Initialize a download context for a given CID.
//
// Loads the manifest from disk and resets streaming state.
// Returns 0 on success, <0 on error.
int download_init(download_ctx* ctx, const char* cid);

// Stream-oriented API:
//
// Fill 'out_buf' with up to 'max_len' bytes of file data.
// Internally walks over manifest chunks (blocks) and slices them.
// Returns:
//   1 -> some data written, *out_len > 0
//   0 -> EOF (no more data)
//  -1 -> error
int download_stream_next(download_ctx* ctx,
                         uint8_t* out_buf,
                         uint32_t max_len,
                         uint32_t* out_len);
