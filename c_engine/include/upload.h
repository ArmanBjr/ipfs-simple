// include/upload.h
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

// Forward declaration for the manifest structure.
struct manifest;

// Forward declaration of upload_ctx (already typedef below, but needed in struct).
typedef struct upload_ctx upload_ctx;

// Per-chunk result used by threaded upload jobs.
struct upload_chunk_result {
    int      ready;     // 0 = not finished, 1 = job completed
    uint32_t size;      // chunk size in bytes
    char*    hash_str;  // multihash base32 string for this chunk
};

// Job payload for a single upload chunk processed by a worker.
typedef struct upload_chunk_job {
    upload_ctx* ctx;
    uint8_t*    data;
    uint32_t    len;
    uint32_t    index;   // logical chunk index
} upload_chunk_job_t;

// Per-connection upload state.
typedef struct upload_ctx {
    // Filename extracted from the UPLOAD_START payload.
    char* filename;

    // Total number of bytes successfully processed into chunks so far.
    uint64_t total_size;

    // Chunk size used for this upload. Usually ENGINE_CHUNK_SIZE.
    uint32_t chunk_size;

    // Sequential chunk index used in the old single-threaded path.
    uint32_t next_chunk_index;

    // Pointer to the manifest object.
    struct manifest* manifest;

    // Streaming buffer for assembling chunks from incoming frames.
    uint8_t*  buffer;
    uint32_t  buffer_len;

    // === Commit-layer state for threaded chunk jobs ===

    // Next logical chunk index to assign to a new job.
    uint32_t next_submit_index;

    // Next chunk index that must be committed into the manifest in order.
    uint32_t next_commit_index;

    // Array of per-chunk results (size + hash) filled by worker jobs.
    struct upload_chunk_result* results;
    uint32_t                    results_capacity; // number of allocated entries

    // Synchronization primitives for coordinating job completion and commit.
    pthread_mutex_t  commit_mutex;
    pthread_cond_t   commit_cond;
} upload_ctx;

// Allocate and initialize a new upload_ctx.
// Returns NULL on failure.
upload_ctx* upload_ctx_create(void);

// Free all resources associated with an upload_ctx.
void upload_ctx_destroy(upload_ctx* ctx);

// Handle the UPLOAD_START frame.
int upload_handle_start(upload_ctx* ctx, const uint8_t* payload, uint32_t len);

// Handle each UPLOAD_CHUNK frame (still old path for now).
int upload_handle_chunk(upload_ctx* ctx, const uint8_t* data, uint32_t len);

// Handle UPLOAD_FINISH.
int upload_handle_finish(upload_ctx* ctx, char** out_cid);

// Worker-side function to process a single upload chunk job.
void upload_chunk_job_run(upload_chunk_job_t* job);
