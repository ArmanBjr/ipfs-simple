// include/threadpool.h
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <stdint.h>
#include "upload.h"   // for upload_chunk_job_t

// Forward declaration for download_ctx to prevent circular include
struct download_ctx;

// Job structure for download chunk job
typedef struct download_chunk_job {
    struct download_ctx* ctx;
    uint32_t index;    // Index of the chunk to be read from blockstore
} download_chunk_job_t;

typedef enum job_type {
    JOB_TYPE_CONNECTION   = 0,
    JOB_TYPE_UPLOAD_CHUNK = 1,
    JOB_TYPE_DOWNLOAD_CHUNK = 2
} job_type_t;

typedef struct job {
    job_type_t type;
    struct job* next;
    union {
        int cfd;                   // For JOB_TYPE_CONNECTION
        upload_chunk_job_t   upload;   // For JOB_TYPE_UPLOAD_CHUNK
        download_chunk_job_t download; // For JOB_TYPE_DOWNLOAD_CHUNK
    } u;
} job_t;

int  threadpool_init(int num_workers);
void threadpool_submit_connection(int cfd);

/* Submit chunk jobs for upload */
int  threadpool_submit_upload_chunk(upload_ctx* ctx,
                                    uint8_t* data,
                                    uint32_t len,
                                    uint32_t index);

/* Submit chunk jobs for download (job only takes index, not actual data) */
int  threadpool_submit_download_chunk(struct download_ctx* ctx,
                                      uint32_t index);

void threadpool_shutdown(void);

#endif
