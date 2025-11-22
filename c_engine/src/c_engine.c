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

#include "engine_config.h"
#include "protocol.h"
#include "upload.h"
#include "download.h"

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

            // Stub behavior for this phase: immediately signal DONE.
            if (send_frame(cfd, OP_DOWNLOAD_DONE, NULL, 0) < 0) {
                fprintf(stderr, "[ERROR] [ENGINE] send_frame(OP_DOWNLOAD_DONE) failed\n");
                close_conn = 1;
            }

            // We can destroy the download context here since we do not stream chunks yet.
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
    if (listen(fd, 64) < 0) {
        perror("listen");
        close(fd);
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
        // Thread-per-connection keeps it readable for OS labs.
        pthread_t th;
        pthread_create(&th, NULL, (void*(*)(void*))handle_connection, (void*)(intptr_t)cfd);
        pthread_detach(th);
    }

    close(fd);
    unlink(g_sock_path);
    return 0;
}
