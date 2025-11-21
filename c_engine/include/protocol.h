// protocol.h
#pragma once

#include <stdint.h>
#include <unistd.h>

ssize_t read_n(int fd, void* buf, size_t n);

int write_all(int fd, const void* buf, size_t n);

int send_frame(int fd, uint8_t op, const void* payload, uint32_t len);


int recv_frame(int fd, uint8_t* op, uint8_t** payload, uint32_t* len);
