// src/threadpool.c
// Thread pool implementation: jobs are "handle this connection" or "process upload chunk".

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

#include "threadpool.h"
#include "upload.h"
#include "download.h"


// We need to call handle_connection() which is defined in c_engine.c
extern void handle_connection(int cfd);

// Global worker array and count
static pthread_t *g_workers     = NULL;
static int        g_num_workers = 0;

// Job queue (simple singly-linked list)
static job_t *g_job_head = NULL;
static job_t *g_job_tail = NULL;

// Synchronization for the job queue
static pthread_mutex_t g_job_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_job_cond  = PTHREAD_COND_INITIALIZER;

// Shutdown flag
static int g_shutdown = 0;

/**
 * Enqueue a job at the tail of the queue.
 * Caller must hold g_job_mutex.
 */
static void enqueue_job(job_t *job) {
    job->next = NULL;
    if (g_job_tail) {
        g_job_tail->next = job;
        g_job_tail = job;
    } else {
        g_job_head = g_job_tail = job;
    }
}

/**
 * Dequeue one job from the head of the queue.
 * Caller must hold g_job_mutex.
 * Returns NULL if the queue is empty.
 */
static job_t *dequeue_job(void) {
    job_t *job = g_job_head;
    if (!job) {
        return NULL;
    }
    g_job_head = job->next;
    if (!g_job_head) {
        g_job_tail = NULL;
    }
    job->next = NULL;
    return job;
}

/**
 * Worker thread main loop.
 * Each worker waits for jobs, takes one, executes it,
 * then repeats until shutdown is requested.
 */
 static void *worker_main(void *arg) {
    (void)arg; // unused

    for (;;) {
        pthread_mutex_lock(&g_job_mutex);

        // Wait until there is a job or shutdown is requested
        while (!g_shutdown && g_job_head == NULL) {
            pthread_cond_wait(&g_job_cond, &g_job_mutex);
        }

        // If shutting down and no jobs left, exit the worker
        if (g_shutdown && g_job_head == NULL) {
            pthread_mutex_unlock(&g_job_mutex);
            break;
        }

        // Get one job from the queue
        job_t *job = dequeue_job();
        pthread_mutex_unlock(&g_job_mutex);

        if (!job) {
            // Should not happen, but guard anyway
            continue;
        }

        switch (job->type) {
        case JOB_TYPE_CONNECTION:
            // handle_connection() will close the cfd at the end.
            handle_connection(job->u.cfd);
            free(job);
            break;

        case JOB_TYPE_UPLOAD_CHUNK: {
            upload_chunk_job_run(&job->u.upload);
            free(job);
            break;
        }

        case JOB_TYPE_DOWNLOAD_CHUNK: {
            download_chunk_job_run(&job->u.download);
            free(job);
            break;
        }

        default:
            fprintf(stderr, "[THREADPOOL] Unknown job type: %d\n", job->type);
            free(job);
            break;
        }
    }

    return NULL;
}


/**
 * Initialize the thread pool with num_workers worker threads.
 * Returns 0 on success, <0 on error.
 */
int threadpool_init(int num_workers) {
    if (num_workers <= 0) {
        fprintf(stderr, "[THREADPOOL] Invalid num_workers: %d\n", num_workers);
        return -1;
    }

    g_shutdown    = 0;
    g_num_workers = num_workers;

    // Allocate worker thread array
    g_workers = (pthread_t *)malloc((size_t)num_workers * sizeof(pthread_t));
    if (!g_workers) {
        fprintf(stderr, "[THREADPOOL] Failed to allocate workers array\n");
        g_num_workers = 0;
        return -1;
    }

    // Create worker threads
    for (int i = 0; i < num_workers; ++i) {
        int rc = pthread_create(&g_workers[i], NULL, worker_main, NULL);
        if (rc != 0) {
            fprintf(stderr, "[THREADPOOL] pthread_create failed for worker %d: %d\n", i, rc);
            // Mark shutdown and join already created workers
            pthread_mutex_lock(&g_job_mutex);
            g_shutdown = 1;
            pthread_cond_broadcast(&g_job_cond);
            pthread_mutex_unlock(&g_job_mutex);

            for (int j = 0; j < i; ++j) {
                pthread_join(g_workers[j], NULL);
            }
            free(g_workers);
            g_workers     = NULL;
            g_num_workers = 0;
            return -1;
        }
    }

    fprintf(stderr, "[THREADPOOL] Initialized with %d workers\n", num_workers);
    return 0;
}

/**
 * Submit a new connection job to the pool.
 * The worker will call handle_connection(cfd).
 */
void threadpool_submit_connection(int cfd) {
    job_t *job = (job_t *)malloc(sizeof(job_t));
    if (!job) {
        fprintf(stderr, "[THREADPOOL] Failed to allocate job for connection\n");
        // We cannot queue this connection, safest is to close it here.
        close(cfd);
        return;
    }

    job->type      = JOB_TYPE_CONNECTION;
    job->u.cfd     = cfd;
    job->next      = NULL;

    pthread_mutex_lock(&g_job_mutex);
    enqueue_job(job);
    pthread_mutex_unlock(&g_job_mutex);

    pthread_cond_signal(&g_job_cond);
}

/**
 * Submit a new upload chunk job to the pool.
 * The worker will call upload_chunk_job_run(&job->u.upload).
 * Returns 0 on success, <0 on error.
 */
int threadpool_submit_upload_chunk(upload_ctx* ctx,
                                   uint8_t*    data,
                                   uint32_t    len,
                                   uint32_t    index) {
    job_t *job = (job_t *)malloc(sizeof(job_t));
    if (!job) {
        fprintf(stderr, "[THREADPOOL] Failed to allocate job for upload chunk\n");
        // We cannot queue this chunk job; free the data buffer to avoid leaks.
        free(data);
        return -1;
    }

    job->type              = JOB_TYPE_UPLOAD_CHUNK;
    job->u.upload.ctx      = ctx;
    job->u.upload.data     = data;
    job->u.upload.len      = len;
    job->u.upload.index    = index;
    job->next              = NULL;

    pthread_mutex_lock(&g_job_mutex);
    enqueue_job(job);
    pthread_mutex_unlock(&g_job_mutex);

    pthread_cond_signal(&g_job_cond);
    return 0;
}


int threadpool_submit_download_chunk(struct download_ctx* ctx, uint32_t index) {
    if (!ctx) {
        fprintf(stderr, "[THREADPOOL] download_chunk: NULL ctx\n");
        return -1;
    }

    job_t *job = (job_t *)malloc(sizeof(job_t));
    if (!job) {
        fprintf(stderr, "[THREADPOOL] Failed to allocate job for download chunk\n");
        return -1;
    }

    job->type             = JOB_TYPE_DOWNLOAD_CHUNK;
    job->u.download.ctx   = ctx;
    job->u.download.index = index;
    job->next             = NULL;

    pthread_mutex_lock(&g_job_mutex);
    enqueue_job(job);
    pthread_mutex_unlock(&g_job_mutex);

    pthread_cond_signal(&g_job_cond);
    return 0;
}



/**
 * Shutdown the thread pool:
 *  - Set shutdown flag
 *  - Wake up all workers
 *  - Join all workers
 *  - Free remaining jobs if any
 *  - Destroy synchronization primitives
 */
void threadpool_shutdown(void) {
    pthread_mutex_lock(&g_job_mutex);
    g_shutdown = 1;
    pthread_cond_broadcast(&g_job_cond);
    pthread_mutex_unlock(&g_job_mutex);

    // Join all workers
    for (int i = 0; i < g_num_workers; ++i) {
        pthread_join(g_workers[i], NULL);
    }

    free(g_workers);
    g_workers     = NULL;
    g_num_workers = 0;

    // Free any remaining jobs in the queue (should normally be empty)
    pthread_mutex_lock(&g_job_mutex);
    job_t *job = g_job_head;
    while (job) {
        job_t *next = job->next;

        if (job->type == JOB_TYPE_UPLOAD_CHUNK) {
            // If a chunk job is still queued, free its data buffer.
            if (job->u.upload.data) {
                free(job->u.upload.data);
            }
        }
        // We do NOT close any connection fd here; unprocessed jobs
        // should not normally remain after shutdown.
        free(job);
        job = next;
    }
    g_job_head = g_job_tail = NULL;
    pthread_mutex_unlock(&g_job_mutex);

    pthread_mutex_destroy(&g_job_mutex);
    pthread_cond_destroy(&g_job_cond);

    fprintf(stderr, "[THREADPOOL] Shutdown complete\n");
}
