// include/threadpool.h
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <stdint.h>
#include "upload.h"   // for upload_chunk_job_t

typedef enum job_type {
    JOB_TYPE_CONNECTION = 0,
    JOB_TYPE_UPLOAD_CHUNK = 1
} job_type_t;

typedef struct job {
    job_type_t type;
    struct job* next;
    union {
        int cfd;
        upload_chunk_job_t upload;   
    } u;
} job_t;


int  threadpool_init(int num_workers);
void threadpool_submit_connection(int cfd);

/* New function to submit chunk jobs */
int  threadpool_submit_upload_chunk(upload_ctx* ctx,
                                    uint8_t* data,
                                    uint32_t len,
                                    uint32_t index);

void threadpool_shutdown(void);

#endif
