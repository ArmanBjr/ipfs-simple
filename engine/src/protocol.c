#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ssize_t read_n(int fd, void* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char*)buf + got, n - got);
        if (r == 0) {
            return 0;
        }
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("read");
            return -1;
        }
        got += (size_t)r;
    }
    return (ssize_t)got;
}

int write_all(int fd, const void* buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = write(fd, (const char*)buf + sent, n - sent);
        if (w < 0) {
            if (errno == EINTR) continue;
            perror("write");
            return -1;
        }
        sent += (size_t)w;
    }
    return 0;
}

int send_frame(int fd, uint8_t op, const void* payload, uint32_t len) {
    uint8_t header[5];
    header[0] = op;

    uint32_t be_len = htonl(len);
    memcpy(header + 1, &be_len, 4);

    if (write_all(fd, header, 5) < 0)
        return -1;

    if (len > 0 && payload != NULL) {
        if (write_all(fd, payload, len) < 0)
            return -1;
    }
    return 0;
}

int recv_frame(int fd, uint8_t* op, uint8_t** payload, uint32_t* len) {
    uint8_t header[5];
    ssize_t r = read_n(fd, header, 5);
    if (r == 0) {
        return 0;
    }
    if (r < 0) {
        return -1;
    }

    *op = header[0];

    uint32_t be_len = 0;
    memcpy(&be_len, header + 1, 4);
    *len = ntohl(be_len);

    if (*len == 0) {
        *payload = NULL;
        return 1;
    }

    *payload = (uint8_t*)malloc(*len);
    if (!*payload) {
        perror("malloc");
        return -1;
    }

    r = read_n(fd, *payload, *len);
    if (r <= 0) {
        free(*payload);
        *payload = NULL;
        return (int)r;
    }

    return 1;
}
