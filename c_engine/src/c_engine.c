// Build: gcc -O2 -pthread -o c_engine c_engine.c
// Run:   ./c_engine /tmp/cengine.sock

#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "locks.h"
#include "engine_config.h"
#include "protocol.h"
#include "upload.h"
#include "download.h"
#include "threadpool.h"
#include "manifest.h"



#define OP_UPLOAD_START   0x01
#define OP_UPLOAD_CHUNK   0x02
#define OP_UPLOAD_FINISH  0x03
#define OP_UPLOAD_DONE    0x81

#define OP_DOWNLOAD_START 0x11
#define OP_DOWNLOAD_CHUNK 0x91
#define OP_DOWNLOAD_DONE  0x92

static const char* g_sock_path = NULL;

void handle_connection(int cfd) {
    upload_ctx*   up   = NULL;
    download_ctx* down = NULL;

    for (;;) {
        uint8_t  op      = 0;
        uint8_t* payload = NULL;
        uint32_t len     = 0;

        int rc = recv_frame(cfd, &op, &payload, &len);
        if (rc == 0) {
            // EOF / client closed connection
            fprintf(stderr, "[ENGINE] connection closed by peer\n");
            break;
        }
        if (rc < 0) {
            fprintf(stderr, "[ERROR] [ENGINE] recv_frame failed\n");
            break;
        }

        int close_conn = 0;

        switch (op) {
        case OP_UPLOAD_START: {
            fprintf(stderr, "[ENGINE] OP_UPLOAD_START received (len=%u)\n", len);

            if (up != NULL) {
                fprintf(stderr,
                        "[ERROR] [UPLOAD] UPLOAD_START received while upload already in progress\n");
                close_conn = 1;
                break;
            }

            up = upload_ctx_create();
            if (!up) {
                fprintf(stderr, "[ERROR] [UPLOAD] failed to create upload_ctx\n");
                close_conn = 1;
                break;
            }

            if (upload_handle_start(up, payload, len) < 0) {
                fprintf(stderr, "[ERROR] [UPLOAD] upload_handle_start failed\n");
                close_conn = 1;
            }

            break;
        }

        case OP_UPLOAD_CHUNK: {
            // CHUNK is only valid if an upload is in progress.
            if (!up) {
                fprintf(stderr,
                        "[ERROR] [UPLOAD] UPLOAD_CHUNK received before UPLOAD_START\n");
                close_conn = 1;
                break;
            }

            if (upload_handle_chunk(up, payload, len) < 0) {
                fprintf(stderr, "[ERROR] [UPLOAD] upload_handle_chunk failed\n");
                close_conn = 1;
            }

            break;
        }

        
        case OP_UPLOAD_FINISH: {
            fprintf(stderr, "[ENGINE] OP_UPLOAD_FINISH received\n");

            if (!up) {
                fprintf(stderr,
                        "[ERROR] [UPLOAD] UPLOAD_FINISH received before UPLOAD_START\n");
                close_conn = 1;
                break;
            }

            char* cid = NULL;
            if (upload_handle_finish(up, &cid) < 0) {
                fprintf(stderr, "[ERROR] [UPLOAD] upload_handle_finish failed\n");
                close_conn = 1;
                break;
            }

            if (!cid) {
                fprintf(stderr, "[ERROR] [UPLOAD] upload_handle_finish returned NULL cid\n");
                close_conn = 1;
                break;
            }

            fprintf(stderr,
                    "[ENGINE] UPLOAD_FINISH -> returning CID %s\n",
                    cid);

            if (send_frame(cfd, OP_UPLOAD_DONE, cid, (uint32_t)strlen(cid)) < 0) {
                fprintf(stderr, "[ERROR] [ENGINE] send_frame(OP_UPLOAD_DONE) failed\n");
                close_conn = 1;
            }

            free(cid);
            upload_ctx_destroy(up);
            up = NULL;

            break;
        }

        case OP_DOWNLOAD_START: {
            fprintf(stderr, "[ENGINE] OP_DOWNLOAD_START received (len=%u)\n", len);

            if (down != NULL) {
                fprintf(stderr,
                        "[ERROR] [DOWNLOAD] DOWNLOAD_START received while download already in progress\n");
                close_conn = 1;
                break;
            }

            down = download_ctx_create();
            if (!down) {
                fprintf(stderr, "[ERROR] [DOWNLOAD] failed to create download_ctx\n");
                close_conn = 1;
                break;
            }

            // Interpret payload as CID string (not null-terminated).
            char* cid_str = NULL;
            if (payload && len > 0) {
                cid_str = (char*)malloc((size_t)len + 1);
                if (!cid_str) {
                    fprintf(stderr,
                            "[ERROR] [DOWNLOAD] failed to allocate cid_str\n");
                    close_conn = 1;
                    break;
                }
                memcpy(cid_str, payload, len);
                cid_str[len] = '\0';
            } else {
                cid_str = strdup("unnamed-cid");
                if (!cid_str) {
                    fprintf(stderr,
                            "[ERROR] [DOWNLOAD] failed to allocate default cid_str\n");
                    close_conn = 1;
                    break;
                }
            }

            if (download_init(down, cid_str) < 0) {
                fprintf(stderr, "[ERROR] [DOWNLOAD] download_init failed\n");
                free(cid_str);
                close_conn = 1;
                break;
            }

            fprintf(stderr,
                    "[ENGINE] DOWNLOAD_START: cid=\"%s\"\n",
                    cid_str);

            free(cid_str);

            if (!down->manifest) {
                fprintf(stderr, "[ERROR] [DOWNLOAD] no manifest after download_init\n");
                close_conn = 1;
                break;
            }

            uint32_t chunk_count = down->manifest->chunk_count;

            // Edge case: empty file (no chunks)
            if (chunk_count == 0) {
                fprintf(stderr, "[ENGINE] DOWNLOAD_START: empty file, sending DONE\n");
                if (send_frame(cfd, OP_DOWNLOAD_DONE, NULL, 0) < 0) {
                    fprintf(stderr,
                            "[ERROR] [ENGINE] send_frame(OP_DOWNLOAD_DONE) failed\n");
                    close_conn = 1;
                }
                download_ctx_destroy(down);
                down = NULL;
                break;
            }

            // Initialize merger state and preallocate result slots
            pthread_mutex_lock(&down->mutex);

            down->next_request_index = 0;
            down->next_send_index    = 0;

            if (down->results_capacity < chunk_count) {
                uint32_t new_cap = chunk_count;
                struct download_chunk_result* new_arr =
                    (struct download_chunk_result*)realloc(
                        down->results,
                        new_cap * sizeof(struct download_chunk_result)
                    );
                if (!new_arr) {
                    fprintf(stderr,
                            "[ERROR] [DOWNLOAD] realloc failed in OP_DOWNLOAD_START\n");
                    pthread_mutex_unlock(&down->mutex);
                    close_conn = 1;
                    break;
                }

                // Initialize new slots
                for (uint32_t i = down->results_capacity; i < new_cap; ++i) {
                    new_arr[i].ready = 0;
                    new_arr[i].data  = NULL;
                    new_arr[i].len   = 0;
                }

                down->results = new_arr;
                down->results_capacity = new_cap;
            }

            // Reset any existing slots (if the context is ever reused)
            for (uint32_t i = 0; i < chunk_count; ++i) {
                if (down->results[i].data) {
                    free(down->results[i].data);
                    down->results[i].data = NULL;
                }
                down->results[i].ready = 0;
                down->results[i].len   = 0;
            }

            pthread_mutex_unlock(&down->mutex);

            // Submit one download job per chunk
            for (uint32_t i = 0; i < chunk_count; ++i) {
                if (threadpool_submit_download_chunk(down, i) < 0) {
                    fprintf(stderr,
                            "[ERROR] [DOWNLOAD] threadpool_submit_download_chunk failed for index=%u\n",
                            i);
                    close_conn = 1;
                    break;
                }
            }

            if (close_conn) {
                break;
            }

            // Sequential merger: send chunks in order 0..chunk_count-1
            while (!close_conn && down->next_send_index < chunk_count) {
                pthread_mutex_lock(&down->mutex);

                // Wait until the next chunk is ready
                while (down->next_send_index < chunk_count &&
                       (down->next_send_index >= down->results_capacity ||
                        down->results[down->next_send_index].ready == 0)) {
                    pthread_cond_wait(&down->cond, &down->mutex);
                }

                if (down->next_send_index >= chunk_count) {
                    pthread_mutex_unlock(&down->mutex);
                    break;
                }

                uint32_t idx = down->next_send_index;
                struct download_chunk_result r = down->results[idx];

                // Clear slot inside the context to avoid double-free on destroy
                down->results[idx].data  = NULL;
                down->results[idx].len   = 0;
                down->results[idx].ready = 0;

                down->next_send_index++;

                pthread_mutex_unlock(&down->mutex);

                if (!r.data || r.len == 0) {
                    fprintf(stderr,
                            "[ERROR] [DOWNLOAD] empty chunk data at index=%u\n",
                            idx);
                    if (r.data) {
                        free(r.data);
                    }
                    close_conn = 1;
                    break;
                }

                if (send_frame(cfd, OP_DOWNLOAD_CHUNK, r.data, r.len) < 0) {
                    fprintf(stderr,
                            "[ERROR] [ENGINE] send_frame(OP_DOWNLOAD_CHUNK) failed at index=%u\n",
                            idx);
                    free(r.data);
                    close_conn = 1;
                    break;
                }

                free(r.data);
            }

            if (!close_conn) {
                if (send_frame(cfd, OP_DOWNLOAD_DONE, NULL, 0) < 0) {
                    fprintf(stderr,
                            "[ERROR] [ENGINE] send_frame(OP_DOWNLOAD_DONE) failed\n");
                    close_conn = 1;
                }
            }

            download_ctx_destroy(down);
            down = NULL;

            break;
        }




        default:
            fprintf(stderr,
                    "[ERROR] [ENGINE] unknown opcode: 0x%02x (len=%u)\n",
                    op, len);
            // Unknown opcode: close the connection to avoid undefined state.
            close_conn = 1;
            break;
        }

        if (payload) {
            free(payload);
            payload = NULL;
        }

        if (close_conn) {
            break;
        }
    }

    if (up != NULL) {
        upload_ctx_destroy(up);
        up = NULL;
    }

    if (down != NULL) {
        download_ctx_destroy(down);
        down = NULL;
    }

    close(cfd);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s /tmp/cengine.sock\n", argv[0]);
        return 2;
    }
    g_sock_path = argv[1];

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 2;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_sock_path, sizeof(addr.sun_path) - 1);
    unlink(g_sock_path);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return 2;
    }
    // Initialize global read/write locks before starting to listen.
    if (locks_init() < 0) {
        fprintf(stderr, "[ENGINE] ERROR: locks_init failed\n");
        close(fd);
        return 2;
    }

    // Initialize thread pool (fixed number of worker threads).
    int num_workers = 4; // you can read this from an env var
    if (threadpool_init(num_workers) < 0) {
        fprintf(stderr, "[ENGINE] ERROR: threadpool_init failed\n");
        close(fd);
        locks_shutdown();
        return 2;
    }

    if (listen(fd, 64) < 0) {
        perror("listen");
        close(fd);
        // locks_init succeeded; we can safely shut locks down.
        threadpool_shutdown();
        locks_shutdown();
        return 2;
    }

    printf("[ENGINE] listening on %s\n", g_sock_path);
    fflush(stdout);

    for (;;) {
        int cfd = accept(fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            break;
        }

        // Submit this connection as a job to the thread pool.
        threadpool_submit_connection(cfd);
    }

    // On error / shutdown, stop accepting new connections and
    // gracefully shut down the thread pool.
    threadpool_shutdown();
    close(fd);
    unlink(g_sock_path);
    locks_shutdown();
    return 0;
}
