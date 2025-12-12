#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "engine_config.h"
#include "util.h"

int util_mkdir_p(const char* path) {
    if (!path || !path[0]) {
        return -1;
    }

    char tmp[ENGINE_MAX_PATH_LEN];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) {
        return -1;
    }

    memcpy(tmp, path, len + 1);

    while (len > 1 && tmp[len - 1] == '/') {
        tmp[--len] = '\0';
    }

    for (char* p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0777) < 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, 0777) < 0 && errno != EEXIST) {
        return -1;
    }

    return 0;
}

int util_join_path(const char* base, const char* sub, char* out, size_t out_size) {
    if (!base || !sub || !out || out_size == 0) {
        return -1;
    }

    size_t bl = strlen(base);
    size_t sl = strlen(sub);

    int need_slash = (bl > 0 && base[bl - 1] != '/');
    size_t total = bl + (need_slash ? 1 : 0) + sl + 1;

    if (total > out_size) {
        return -1;
    }

    memcpy(out, base, bl);
    size_t pos = bl;

    if (need_slash) {
        out[pos++] = '/';
    }

    memcpy(out + pos, sub, sl);
    pos += sl;
    out[pos] = '\0';

    return 0;
}

static void vlog_impl(FILE* stream, const char* prefix, const char* fmt, va_list ap) {
    if (prefix) {
        fputs(prefix, stream);
    }
    vfprintf(stream, fmt, ap);
    fputc('\n', stream);
}

void log_info(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog_impl(stderr, "[INFO] ", fmt, ap);
    va_end(ap);
}

void log_error(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog_impl(stderr, "[ERROR] ", fmt, ap);
    va_end(ap);
}
