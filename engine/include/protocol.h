// protocol.h
#pragma once

#include <stdint.h>
#include <unistd.h>

// opcodes
#define OP_UPLOAD_START    0x01
#define OP_UPLOAD_CHUNK    0x02
#define OP_UPLOAD_FINISH   0x03
#define OP_UPLOAD_RESUME   0x04
#define OP_AUTH            0x05
#define OP_UPLOAD_DONE     0x81
#define OP_DELETE_FILE     0x20

#define OP_DOWNLOAD_START  0x11
#define OP_DOWNLOAD_CHUNK  0x91
#define OP_DOWNLOAD_DONE   0x92

#define OP_LIST_FILES      0x30
#define OP_LIST_RESPONSE   0xB0

ssize_t read_n(int fd, void* buf, size_t n);

int write_all(int fd, const void* buf, size_t n);

int send_frame(int fd, uint8_t op, const void* payload, uint32_t len);


int recv_frame(int fd, uint8_t* op, uint8_t** payload, uint32_t* len);
