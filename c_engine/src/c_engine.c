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
#include <dirent.h>
#define _POSIX_C_SOURCE 200809L
#include <signal.h>

#include "locks.h"
#include "engine_config.h"
#include "protocol.h"
#include "upload.h"
#include "download.h"
#include "threadpool.h"
#include "manifest.h"
#include "util.h"



static const char* g_sock_path = NULL;

void handle_connection(int cfd) {
    upload_ctx*   up   = NULL;
    download_ctx* down = NULL;
    char* conn_auth_token = NULL;  


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
            case OP_AUTH: {
                if (conn_auth_token != NULL) {
                    fprintf(stderr, "[ENGINE] OP_AUTH received multiple times, ignoring\n");
                    break;
                }
            
                if (!payload || len == 0) {
                    fprintf(stderr, "[AUTH] OP_AUTH received with empty token\n");
                    close_conn = 1;
                    break;
                }
            
                conn_auth_token = strndup((char*)payload, len);
                if (!conn_auth_token) {
                    fprintf(stderr, "[AUTH] failed to allocate auth token\n");
                    close_conn = 1;
                    break;
                }
            
                fprintf(stderr, "[AUTH] Received token: %s\n", conn_auth_token);
                break;
            }
        case OP_UPLOAD_START: {
            fprintf(stderr, "[ENGINE] OP_UPLOAD_START received (len=%u)\n", len);

            if (up != NULL) {
                fprintf(stderr,
                        "[ERROR] [UPLOAD] UPLOAD_START received while upload already in progress\n");
                close_conn = 1;
                break;
            }

            up = upload_ctx_create();

            up->auth_token = conn_auth_token ? strdup(conn_auth_token) : NULL;
            
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
        

        case OP_UPLOAD_RESUME: {
            if (up != NULL) {
                fprintf(stderr, "[ERROR] [UPLOAD] UPLOAD_RESUME received while upload already in progress\n");
                close_conn = 1;
                break;
            }
        
            if (handle_upload_resume(&up, conn_auth_token, payload, len, cfd) < 0) {
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
            int finish_rc = upload_handle_finish(up, &cid);

            if (finish_rc < 0) {
                fprintf(stderr, "[ERROR] [UPLOAD] upload_handle_finish failed (rc=%d)\n", finish_rc);

                if (finish_rc == -2 || cid == NULL) {
                    const char* err_msg = "UPLOAD_ERROR";
                    send_frame(cfd, OP_UPLOAD_DONE, err_msg, (uint32_t)strlen(err_msg));
                }

                upload_ctx_destroy(up);
                up = NULL;
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

        case OP_DELETE_FILE: {
            char* cid_str = NULL;
            if (payload && len > 0) {
                cid_str = strndup((const char*)payload, len);
            }
        
            if (!cid_str) {
                fprintf(stderr, "[ERROR] DELETE_FILE: invalid CID\n");
                close_conn = 1;
                break;
            }
        
            
            char owner_path[ENGINE_MAX_PATH_LEN];
            snprintf(owner_path, sizeof(owner_path), "owners/%s.owner", cid_str);
        
            FILE* f = fopen(owner_path, "r");
            if (f) {
                char owner_token[256];
                if (fgets(owner_token, sizeof(owner_token), f)) {
                    owner_token[strcspn(owner_token, "\r\n")] = '\0';  // Trim newline

                    if (strncmp(owner_token, "owner-", 6) == 0) {
                        if (!conn_auth_token || strcmp(owner_token, conn_auth_token) != 0) {
                            fprintf(stderr, "[AUTH] Token mismatch: delete forbidden for CID=%s\n", cid_str);
                            fclose(f);
                            const char* err_msg = "AUTH_ERROR";
                            send_frame(cfd, OP_UPLOAD_DONE, err_msg, (uint32_t)strlen(err_msg));
                            free(cid_str);
                            close_conn = 1;
                            break;
                        }
                    }
                }
                fclose(f);
            } else {
                if (conn_auth_token != NULL) {
                    fprintf(stderr, "[AUTH] Delete not allowed: no owner file for CID=%s\n", cid_str);
                    const char* err_msg = "AUTH_ERROR";
                    send_frame(cfd, OP_UPLOAD_DONE, err_msg, (uint32_t)strlen(err_msg));
                    free(cid_str);
                    close_conn = 1;
                    break;
                }
            }

        
        
            if (manifest_delete(cid_str) < 0) {
                fprintf(stderr, "[DELETE] failed to delete manifest and chunks for CID=%s\n", cid_str);
            } else {
                fprintf(stderr, "[DELETE] deleted manifest and (possibly) orphaned chunks for CID=%s\n", cid_str);
        
                unlink(owner_path);
            }
        
            free(cid_str);
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
            down->auth_token = conn_auth_token ? strdup(conn_auth_token) : NULL;
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
            
            char owner_path[ENGINE_MAX_PATH_LEN];
            snprintf(owner_path, sizeof(owner_path), "owners/%s.owner", cid_str);

            FILE* f = fopen(owner_path, "r");
            if (f) {
                char owner_token[256];
                if (fgets(owner_token, sizeof(owner_token), f)) {
                    owner_token[strcspn(owner_token, "\r\n")] = '\0';  // Trim newline

                    if (strncmp(owner_token, "owner-", 6) == 0) {
                        if (!conn_auth_token || strcmp(owner_token, conn_auth_token) != 0) {
                            fprintf(stderr, "[AUTH] Token mismatch: download forbidden for CID=%s\n", cid_str);
                            fclose(f);
                            const char* err_msg = "AUTH_ERROR";
                            send_frame(cfd, OP_DOWNLOAD_DONE, err_msg, (uint32_t)strlen(err_msg));
                            close_conn = 1;
                            free(cid_str);
                            break;
                        }
                    }
                }
                fclose(f);
            }


            if (download_init(down, cid_str) < 0) {
                const char* err_msg = "DOWNLOAD_ERROR";
                send_frame(cfd, OP_DOWNLOAD_DONE, err_msg, (uint32_t)strlen(err_msg));
                fprintf(stderr, "[WARN] [DOWNLOAD] Invalid CID: \"%s\" (download_init failed)\n", cid_str);
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




        case OP_LIST_FILES: {
            fprintf(stderr, "[ENGINE] OP_LIST_FILES received\n");
            
            if (!conn_auth_token) {
                fprintf(stderr, "[AUTH] LIST_FILES without auth token\n");
                const char* err_msg = "[]";
                send_frame(cfd, OP_LIST_RESPONSE, err_msg, (uint32_t)strlen(err_msg));
                break;
            }

            char json_buf[1024 * 64] = "[";
            int json_len = 1;
            int first = 1;

            DIR* dir = opendir("owners");
            if (dir) {
                struct dirent* entry;
                while ((entry = readdir(dir)) != NULL) {
                    if (strstr(entry->d_name, ".owner") == NULL) continue;

                    char owner_path[ENGINE_MAX_PATH_LEN];
                    snprintf(owner_path, sizeof(owner_path), "owners/%s", entry->d_name);

                    FILE* f = fopen(owner_path, "r");
                    if (!f) continue;

                    char token[256];
                    if (!fgets(token, sizeof(token), f)) {
                        fclose(f);
                        continue;
                    }
                    fclose(f);
                    
                    token[strcspn(token, "\r\n")] = '\0';
                    if (strcmp(token, conn_auth_token) != 0) continue;

                    char cid[128];
                    strncpy(cid, entry->d_name, sizeof(cid) - 1);
                    cid[sizeof(cid) - 1] = '\0';
                    char* dot = strstr(cid, ".owner");
                    if (dot) *dot = '\0';

                    char manifest_path[ENGINE_MAX_PATH_LEN];
                    snprintf(manifest_path, sizeof(manifest_path), "manifests/%s.json", cid);

                    FILE* mf = fopen(manifest_path, "r");
                    char filename[256] = "";
                    uint64_t filesize = 0;
                    
                    if (mf) {
                        char line[512];
                        while (fgets(line, sizeof(line), mf)) {
                            if (strstr(line, "\"filename\"")) {
                                char* start = strchr(line, ':');
                                if (start) {
                                    start = strchr(start, '"');
                                    if (start) {
                                        start++;
                                        char* end = strchr(start, '"');
                                        if (end) {
                                            int len = end - start;
                                            if (len > 0 && len < (int)sizeof(filename)) {
                                                strncpy(filename, start, len);
                                                filename[len] = '\0';
                                            }
                                        }
                                    }
                                }
                            } else if (strstr(line, "\"total_size\"")) {
                                char* start = strchr(line, ':');
                                if (start) {
                                    filesize = strtoull(start + 1, NULL, 10);
                                }
                            }
                        }
                        fclose(mf);
                    }

                    if (!first) {
                        json_buf[json_len++] = ',';
                    }
                    first = 0;

                    int n = snprintf(json_buf + json_len, sizeof(json_buf) - json_len,
                                     "{\"cid\":\"%s\",\"filename\":\"%s\",\"size\":%llu}",
                                     cid, filename[0] ? filename : cid, (unsigned long long)filesize);
                    if (n > 0) json_len += n;
                }
                closedir(dir);
            }

            json_buf[json_len++] = ']';
            json_buf[json_len] = '\0';

            send_frame(cfd, OP_LIST_RESPONSE, json_buf, json_len);
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

    if (conn_auth_token) {
        free(conn_auth_token);
        conn_auth_token = NULL;
    }
}

int main(int argc, char** argv) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
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

    // Initialize thread pool 
    int num_workers = 16;  

    long cpus = sysconf(_SC_NPROCESSORS_ONLN);  
    fprintf(stderr, "[ENGINE] auto-detected number of workers: %ld\n", cpus);
    if (cpus > 0 && cpus <= 256) {
        num_workers = (int)cpus;
    }

    const char* env = getenv("CENGINE_WORKERS");
    if (env != NULL) {
        int parsed = atoi(env);
        if (parsed > 0 && parsed <= 256) {
            num_workers = parsed;  
        } else {
            fprintf(stderr, "[ENGINE] Invalid CENGINE_WORKERS env value: %s, using auto-detected=%d\n", env, num_workers);
        }
    }

    if (util_mkdir_p("manifests/in-progress") < 0) {
        fprintf(stderr, "[ENGINE] ERROR: failed to create in-progress manifest dir\n");
        return 2;
    }

    if (threadpool_init(num_workers) < 0) {
        fprintf(stderr, "[ENGINE] ERROR: threadpool_init failed\n");
        close(fd);
        locks_shutdown();
        return 2;
    }

    if (util_mkdir_p("owners") < 0) {
        fprintf(stderr, "[ENGINE] ERROR: failed to create owners directory\n");
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
