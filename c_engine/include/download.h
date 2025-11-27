// include/download.h
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>   // for mutex / cond

// Forward declaration for the manifest structure.
struct manifest;

// Result for each chunk downloaded by a job
typedef struct download_chunk_result {
    int      ready;   // 0 = not ready yet, 1 = ready (can use -1 = error in the future)
    uint8_t* data;    // buffer for the data of this chunk (malloc'ed)
    uint32_t len;     // actual data length
} download_chunk_result;

// Per-connection download state.
// A new download_ctx is created at DOWNLOAD_START and destroyed when
// the download completes or the connection closes.
typedef struct download_ctx {
    // CID of the file being downloaded (malloc'ed string).
    char* cid;

    // Loaded manifest for this CID.
    struct manifest* manifest;

    // --- OLD: single-threaded streaming state (kept for now, will be revised in 2.D.4) ---
    // Index of the next manifest chunk (block) that should be used.
    // This walks over manifest->chunks[].
    uint32_t current_index;

    // We keep the currently loaded block (one manifest chunk) here
    // and slice it into smaller pieces for each OP_DOWNLOAD_CHUNK frame.
    uint8_t* cur_block;      // malloc'ed buffer holding current block data
    uint32_t cur_block_len;  // total bytes in cur_block
    uint32_t cur_block_pos;  // how many bytes already sent from cur_block

    // --- NEW: state for multi-threaded download + ordered merge ---

    // The next index we want to request (for pre-windowing, or full pre-submit)
    uint32_t next_request_index;

    // The next index we need to send to the client (ordered merge)
    uint32_t next_send_index;

    // Array of chunk results (at least of length chunk_count)
    download_chunk_result* results;
    uint32_t results_capacity;   // usually == chunk_count

    // Mutex and condition for coordinating worker jobs and merge thread
    pthread_mutex_t mutex;
    pthread_cond_t  cond;

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

// Stream-oriented API (currently single-threaded version; will be replaced by job-based model later):
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

// Forward declaration for download chunk job (defined in threadpool.h)
struct download_chunk_job;

// Worker entry for download chunk jobs.
// This is called from the thread pool workers.
void download_chunk_job_run(struct download_chunk_job* job);
