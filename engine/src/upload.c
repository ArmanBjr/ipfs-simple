// src/upload.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include "engine_config.h"
#include "upload.h"

#include "manifest.h"
#include "hash.h"
#include "blockstore.h"
#include <pthread.h>
#include "threadpool.h"
#include <unistd.h>     
#include "util.h"       
#include "protocol.h"

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

    // Initialize commit-layer state (used by threaded upload mode).
    ctx->next_submit_index = 0;
    ctx->next_commit_index = 0;
    ctx->results           = NULL;
    ctx->results_capacity  = 0;

    if (pthread_mutex_init(&ctx->commit_mutex, NULL) != 0) {
        fprintf(stderr, "[UPLOAD] ERROR: pthread_mutex_init(commit_mutex) failed\n");
        free(ctx->buffer);
        free(ctx);
        return NULL;
    }

    if (pthread_cond_init(&ctx->commit_cond, NULL) != 0) {
        fprintf(stderr, "[UPLOAD] ERROR: pthread_cond_init(commit_cond) failed\n");
        pthread_mutex_destroy(&ctx->commit_mutex);
        free(ctx->buffer);
        free(ctx);
        return NULL;
    }

    fprintf(stderr,
            "[UPLOAD] upload_ctx created (chunk_size=%u)\n",
            ctx->chunk_size);
    return ctx;
}


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

    // Free streaming buffer
    if (ctx->buffer) {
        free(ctx->buffer);
        ctx->buffer = NULL;
    }

    // Free per-chunk results (if any)
    if (ctx->results) {
        for (uint32_t i = 0; i < ctx->results_capacity; ++i) {
            if (ctx->results[i].hash_str) {
                free(ctx->results[i].hash_str);
                ctx->results[i].hash_str = NULL;
            }
        }
        free(ctx->results);
        ctx->results = NULL;
        ctx->results_capacity = 0;
    }

    // Destroy synchronization primitives
    pthread_mutex_destroy(&ctx->commit_mutex);
    pthread_cond_destroy(&ctx->commit_cond);

    if (ctx->auth_token) {
        free(ctx->auth_token);
        ctx->auth_token = NULL;
    }

    if (ctx->upload_id) {
        free(ctx->upload_id);
        ctx->upload_id = NULL;
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

    ctx->declared_total_size = declared_total_size;
    ctx->total_size          = 0;  


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
    if (ctx->auth_token) {
        ctx->manifest->auth_token = strdup(ctx->auth_token);
    }

    
    if (!ctx->manifest) {
        fprintf(stderr, "[UPLOAD] ERROR: manifest_create failed\n");
        return -1;
    }

    fprintf(stderr,
            "[UPLOAD] start: filename=\"%s\", declared_total_size=%llu, chunk_size=%u (manifest created)\n",
            ctx->filename,
            (unsigned long long)declared_total_size,
            ctx->chunk_size);


    ctx->declared_total_size = declared_total_size;

    // Generate upload_id from filename + timestamp
    char buf[256];
    snprintf(buf, sizeof(buf), "%s-%ld", ctx->filename ? ctx->filename : "file", time(NULL));
    ctx->upload_id = strdup(buf);

    return 0;
}


// // Process a single, full chunk of data.
// // - compute content hash
// // - convert to multihash (base32 string)
// // - store block in the blockstore
// // - append chunk entry to the manifest
// static int upload_process_full_chunk(upload_ctx* ctx, const uint8_t* data, uint32_t len) {
//     if (!ctx || !data || len == 0) {
//         fprintf(stderr, "[UPLOAD] ERROR: upload_process_full_chunk: invalid arguments\n");
//         return -1;
//     }
//     if (!ctx->manifest) {
//         fprintf(stderr, "[UPLOAD] ERROR: upload_process_full_chunk: manifest is NULL\n");
//         return -1;
//     }

//     hash_algo_t algo = hash_algo_from_env();
//     hash_result_t h;
//     memset(&h, 0, sizeof(h));

//     if (hash_compute(algo, data, (size_t)len, &h) < 0) {
//         fprintf(stderr, "[UPLOAD] ERROR: failed to compute chunk hash\n");
//         return -1;
//     }

//     char* mh_str = NULL;
//     if (hash_to_multihash_b32(&h, &mh_str) < 0 || !mh_str) {
//         fprintf(stderr, "[UPLOAD] ERROR: failed to convert hash to multihash base32\n");
//         hash_result_free(&h);
//         return -1;
//     }

//     // Store the block in the blockstore (deduplicated inside blockstore_put).
//     if (blockstore_put(mh_str, data, (size_t)len) < 0) {
//         fprintf(stderr, "[UPLOAD] ERROR: blockstore_put failed for chunk %u\n", ctx->next_chunk_index);
//         hash_result_free(&h);
//         free(mh_str);
//         return -1;
//     }

//     // Record this chunk in the manifest.
//     if (manifest_add_chunk(ctx->manifest, ctx->next_chunk_index, len, mh_str) < 0) {
//         fprintf(stderr, "[UPLOAD] ERROR: manifest_add_chunk failed for chunk %u\n", ctx->next_chunk_index);
//         hash_result_free(&h);
//         free(mh_str);
//         return -1;
//     }

//     // Update counters: we still keep total_size as "bytes actually chunked".
//     ctx->total_size      += (uint64_t)len;
//     ctx->next_chunk_index++;

//     hash_result_free(&h);
//     free(mh_str);

//     fprintf(stderr,
//         "[UPLOAD] processed full chunk index=%u, size=%u (total_size=%" PRIu64 ")\n",
//         ctx->next_chunk_index - 1, len, ctx->total_size);

//     return 0;
// }

// Worker-side function: process a single chunk job in a background thread.
// - Compute the hash and convert it to a multihash string
// - Store the block in the blockstore
// - Record the result into ctx->results[index] under commit_mutex
// - Signal commit_cond so the connection thread can commit in order
// - Finally, free the job->data buffer and the job struct itself.
void upload_chunk_job_run(upload_chunk_job_t* job) {
    if (!job) {
        fprintf(stderr, "[UPLOAD] ERROR: upload_chunk_job_run called with NULL job\n");
        return;
    }

    upload_ctx* ctx   = job->ctx;
    uint8_t*    data  = job->data;
    uint32_t    len   = job->len;
    uint32_t    index = job->index;

    if (!ctx || !data || len == 0) {
        fprintf(stderr, "[UPLOAD] ERROR: upload_chunk_job_run: invalid arguments (ctx=%p, data=%p, len=%u)\n",
                (void*)ctx, (void*)data, len);
        if (data) {
            free(data);
        }
        return;
    }

    if (!ctx->manifest) {
        fprintf(stderr, "[UPLOAD] ERROR: upload_chunk_job_run: ctx->manifest is NULL\n");
        free(data);
        return;
    }

    // 1) Compute hash over the chunk
    hash_algo_t   algo = hash_algo_from_env();
    hash_result_t h;
    memset(&h, 0, sizeof(h));

    if (hash_compute(algo, data, (size_t)len, &h) < 0) {
        fprintf(stderr, "[UPLOAD] ERROR: upload_chunk_job_run: failed to compute chunk hash (index=%u)\n", index);
        free(data);
        return;
    }

    char* mh_str = NULL;
    if (hash_to_multihash_b32(&h, &mh_str) < 0 || !mh_str) {
        fprintf(stderr, "[UPLOAD] ERROR: upload_chunk_job_run: failed to convert hash to multihash base32 (index=%u)\n", index);
        hash_result_free(&h);
        free(data);
        return;
    }

    // 2) Store the block in the blockstore (deduplicated by blockstore_put)
    if (blockstore_put(mh_str, data, (size_t)len) < 0) {
        fprintf(stderr, "[UPLOAD] ERROR: upload_chunk_job_run: blockstore_put failed (index=%u)\n", index);
        hash_result_free(&h);
        free(mh_str);
        free(data);
        return;
    }

    // 3) Record result in ctx->results[index] under commit_mutex
    pthread_mutex_lock(&ctx->commit_mutex);

    // Ensure results array is large enough
    if (ctx->results_capacity <= index) {
        uint32_t new_cap = ctx->results_capacity ? ctx->results_capacity : 16;
        while (new_cap <= index) {
            new_cap *= 2;
        }

        struct upload_chunk_result* new_arr =
            (struct upload_chunk_result*)realloc(ctx->results,
                                                 new_cap * sizeof(struct upload_chunk_result));
        if (!new_arr) {
            fprintf(stderr, "[UPLOAD] ERROR: upload_chunk_job_run: realloc(results) failed\n");
            pthread_mutex_unlock(&ctx->commit_mutex);
            hash_result_free(&h);
            free(mh_str);
            free(data);
            return;
        }

        // Initialize newly added entries
        for (uint32_t i = ctx->results_capacity; i < new_cap; ++i) {
            new_arr[i].ready    = 0;
            new_arr[i].size     = 0;
            new_arr[i].hash_str = NULL;
        }

        ctx->results          = new_arr;
        ctx->results_capacity = new_cap;
    }

    struct upload_chunk_result* r = &ctx->results[index];

    // If there was any previous hash_str (should not happen in normal flow), free it.
    if (r->hash_str) {
        free(r->hash_str);
        r->hash_str = NULL;
    }

    r->size = len;
    r->hash_str = strdup(mh_str);
    if (!r->hash_str) {
        fprintf(stderr, "[UPLOAD] ERROR: upload_chunk_job_run: strdup(mh_str) failed (index=%u)\n", index);
        // Leave ready=0 so commit side will not wait on this as "done".
        r->ready = 0;
    } else {
        r->ready = 1;
    }

    // Wake up any thread waiting to commit chunks.
    pthread_cond_broadcast(&ctx->commit_cond);
    pthread_mutex_unlock(&ctx->commit_mutex);

    // 4) آماده‌کردن preview برای لاگ قبل از آزاد کردن mh_str
    char hash_preview[16];
    hash_preview[0] = '\0';
    if (mh_str) {
        snprintf(hash_preview, sizeof(hash_preview), "%.*s", 10, mh_str);
    }

    hash_result_free(&h);
    free(mh_str);
    free(data);

    fprintf(stderr,
        "[UPLOAD] worker processed chunk index=%u, size=%u, hash=%s..., filename=\"%s\", ctx=%p\n",
        index, len,
        hash_preview,
        ctx->filename ? ctx->filename : "(null)",
        (void*)ctx);
}



// High-level stream handler:
// Accepts arbitrary-sized pieces of the upload stream, buffering until a full chunk is ready.
// Once the buffer accumulates ctx->chunk_size bytes, it is submitted as a job to the thread pool.
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
        uint32_t to_copy    = (remaining < free_space) ? remaining : free_space;

        // Copy incoming data into the staging buffer.
        memcpy(ctx->buffer + ctx->buffer_len, p, to_copy);
        ctx->buffer_len += to_copy;
        p               += to_copy;
        remaining       -= to_copy;

        // When the buffer becomes a full chunk, submit it as a job to the thread pool.
        if (ctx->buffer_len == ctx->chunk_size) {
            // Allocate an independent copy for the worker.
            uint8_t* chunk_copy = (uint8_t*)malloc(ctx->chunk_size);
            if (!chunk_copy) {
                fprintf(stderr, "[UPLOAD] ERROR: upload_handle_stream_data: malloc(chunk_copy) failed\n");
                ctx->buffer_len = 0;  // drop buffered data on error
                return -1;
            }
            memcpy(chunk_copy, ctx->buffer, ctx->chunk_size);

            // Assign a logical chunk index and submit job.
            uint32_t index = ctx->next_submit_index++;
            if (threadpool_submit_upload_chunk(ctx, chunk_copy, ctx->chunk_size, index) < 0) {
                fprintf(stderr,
                        "[UPLOAD] ERROR: upload_handle_stream_data: threadpool_submit_upload_chunk failed (index=%u)\n",
                        index);
                // threadpool_submit_upload_chunk already freed chunk_copy on failure.
                ctx->buffer_len = 0;
                return -1;
            }

            // Reset buffer for the next chunk.
            ctx->buffer_len = 0;
        }
    }

    return 0;
}


// Public entry point used by engine.c for each OP_UPLOAD_CHUNK frame.
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

    // If there is a final partial chunk in the buffer, submit it as a job.
    if (ctx->buffer && ctx->buffer_len > 0) {
        fprintf(stderr,
                "[UPLOAD] finish: flushing final partial chunk as job (size=%u)\n",
                ctx->buffer_len);

        uint8_t* tail_copy = (uint8_t*)malloc(ctx->buffer_len);
        if (!tail_copy) {
            fprintf(stderr, "[UPLOAD] ERROR: upload_handle_finish: malloc(tail_copy) failed\n");
            return -1;
        }
        memcpy(tail_copy, ctx->buffer, ctx->buffer_len);

        uint32_t index = ctx->next_submit_index++;
        if (threadpool_submit_upload_chunk(ctx, tail_copy, ctx->buffer_len, index) < 0) {
            fprintf(stderr,
                    "[UPLOAD] ERROR: upload_handle_finish: threadpool_submit_upload_chunk failed for final chunk (index=%u)\n",
                    index);
            // threadpool_submit_upload_chunk already freed tail_copy on failure.
            return -1;
        }

        // Buffer is now logically consumed.
        ctx->buffer_len = 0;
    }

    // At this point, all chunks 0..next_submit_index-1 have been submitted as jobs.
    // Now we must wait until all of them are completed and commit them in order.
    pthread_mutex_lock(&ctx->commit_mutex);

    while (ctx->next_commit_index < ctx->next_submit_index) {
        uint32_t idx = ctx->next_commit_index;

        // Wait until this specific chunk index becomes ready.
        while ((idx >= ctx->results_capacity) ||
               (ctx->results[idx].ready == 0)) {
            pthread_cond_wait(&ctx->commit_cond, &ctx->commit_mutex);
        }

        struct upload_chunk_result* r = &ctx->results[idx];

        // Commit this chunk into the manifest.
        if (manifest_add_chunk(ctx->manifest, idx, r->size, r->hash_str) < 0) {
            fprintf(stderr,
                    "[UPLOAD] ERROR: upload_handle_finish: manifest_add_chunk failed for chunk %u\n",
                    idx);
            pthread_mutex_unlock(&ctx->commit_mutex);
            return -1;
        }

        manifest_save_progress(ctx->manifest, ctx->upload_id);


        ctx->total_size += (uint64_t)r->size;

        fprintf(stderr,
            "[UPLOAD] committed chunk index=%u, size=%u, hash=%.*s..., filename=\"%s\", ctx=%p, (total_size=%" PRIu64 ")\n",
            idx, r->size,
            10, r->hash_str ? r->hash_str : "(null)",
            ctx->filename ? ctx->filename : "(null)",
            (void*)ctx,
            ctx->total_size);
    

        // We have copied the hash into the manifest; we no longer need it here.
        if (r->hash_str) {
            free(r->hash_str);
            r->hash_str = NULL;
        }
        r->ready = 0;

        // Maintain next_commit_index and next_chunk_index for logging.
        ctx->next_commit_index++;
        ctx->next_chunk_index++;
    }

    pthread_mutex_unlock(&ctx->commit_mutex);


    if (ctx->declared_total_size != 0 &&
        ctx->total_size != ctx->declared_total_size) {

        fprintf(stderr,
                "[UPLOAD ERROR] received size (%" PRIu64 ") does not match declared size (%" PRIu64 ") for file \"%s\", ctx=%p\n",
                ctx->total_size,
                ctx->declared_total_size,
                ctx->filename ? ctx->filename : "(null)",
                (void*)ctx);

        manifest_free(ctx->manifest);
        ctx->manifest = NULL;

        if (ctx->upload_id) {
            char path[ENGINE_MAX_PATH_LEN];
            snprintf(path, sizeof(path), "manifests/in-progress/%s.json", ctx->upload_id);
            unlink(path);
        }

        *out_cid = NULL;
        return -2;
    }


    // Now all chunks have been committed to the manifest in order.
    manifest_finalize(ctx->manifest, ctx->total_size);

    // Save manifest to disk and obtain CID.
    char* cid = NULL;
    if (manifest_save_and_get_cid(ctx->manifest, &cid) < 0 || !cid) {
        fprintf(stderr, "[UPLOAD] ERROR: manifest_save_and_get_cid failed\n");
        return -1;
    }

    if (ctx->upload_id) {
        char path[ENGINE_MAX_PATH_LEN];
        snprintf(path, sizeof(path), "manifests/in-progress/%s.json", ctx->upload_id);
        unlink(path);
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


int handle_upload_resume(upload_ctx** out_up, const char* auth_token, const uint8_t* payload, uint32_t len, int cfd) {
    if (!out_up || !payload || len == 0) {
        log_error("[RESUME] invalid args");
        const char* err = "RESUME_ERROR: invalid arguments";
        send_frame(cfd, OP_UPLOAD_DONE, err, (uint32_t)strlen(err));
        return -1;
    }

    char* upload_id = strndup((const char*)payload, len);
    if (!upload_id) {
        const char* err = "RESUME_ERROR: memory error";
        send_frame(cfd, OP_UPLOAD_DONE, err, (uint32_t)strlen(err));
        return -1;
    }

    manifest* m = manifest_load_in_progress(upload_id);
    if (!m) {
        log_error("[RESUME] could not load manifest for upload_id=%s", upload_id);
        free(upload_id);
        const char* err = "RESUME_ERROR: no manifest found for upload_id";
        send_frame(cfd, OP_UPLOAD_DONE, err, (uint32_t)strlen(err));
        return -1;
    }

    upload_ctx* ctx = upload_ctx_create();
    if (!ctx) {
        manifest_free(m);
        free(upload_id);
        const char* err = "RESUME_ERROR: failed to create upload context";
        send_frame(cfd, OP_UPLOAD_DONE, err, (uint32_t)strlen(err));
        return -1;
    }

    ctx->upload_id   = upload_id;
    ctx->manifest    = m;
    ctx->filename    = strdup(m->filename);
    ctx->chunk_size  = m->chunk_size;
    ctx->total_size  = m->total_size;
    ctx->next_chunk_index   = m->chunk_count;
    ctx->next_submit_index  = m->chunk_count;
    ctx->next_commit_index  = m->chunk_count;
    ctx->declared_total_size = 0;

    if (auth_token) {
        ctx->auth_token = strdup(auth_token);
    }

    *out_up = ctx;

    log_info("[RESUME] Resumed upload_id=%s (%u chunks already uploaded)", upload_id, m->chunk_count);

    const char* ok = "RESUME_OK";
    send_frame(cfd, OP_UPLOAD_DONE, ok, (uint32_t)strlen(ok));

    return 0;
}