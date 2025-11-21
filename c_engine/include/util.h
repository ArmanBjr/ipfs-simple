// util.h
#pragma once

#include <stddef.h>

// Recursive mkdir (mkdir -p behavior)
// If directory already exists, return success
int util_mkdir_p(const char* path);

// Join base path and subpath into out buffer
// out must have enough space (e.g., ENGINE_MAX_PATH_LEN)
int util_join_path(const char* base,
                   const char* sub,
                   char* out,
                   size_t out_size);

// Basic logging utilities (optional)
void log_info(const char* fmt, ...);
void log_error(const char* fmt, ...);
