// download.h
#pragma once

#include <stdint.h>

// Forward declaration of manifest (defined in manifest.h)
struct manifest;

// Download context for a single connection
typedef struct download_ctx {
    char*            cid;           // requested CID
    struct manifest* manifest;      // loaded manifest
    uint32_t         next_index;    // next chunk index to send
} download_ctx;

// Create / destroy download context
download_ctx* download_ctx_create(void);
void          download_ctx_destroy(download_ctx* ctx);

// Initialize download: load manifest from disk by CID
int download_init(download_ctx* ctx, const char* cid);

// Get next chunk:
// return 1 → chunk available (caller must free(out_data))
// return 0 → no more chunks (download completed)
// return <0 → error
int download_next_chunk(download_ctx* ctx, uint8_t** out_data, uint32_t* out_len);
