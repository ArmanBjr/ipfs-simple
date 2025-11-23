// blockstore_test.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "engine_config.h"
#include "blockstore.h"

int main(void) {
    printf("[TEST] blockstore test start\n");

    // Initialize blockstore at default root (ENGINE_BLOCKS_DIR, e.g. "blocks")
    if (blockstore_init(NULL) < 0) {
        printf("[TEST] blockstore_init failed\n");
        return 1;
    }

    const char* hash = "abcd1234dummyhash";
    const char* text = "HELLO_MONIBA_<3";

    printf("[TEST] putting block \"%s\"…\n", hash);
    if (blockstore_put(hash, (const uint8_t*)text, strlen(text)) < 0) {
        printf("[TEST] blockstore_put failed\n");
        return 1;
    }

    int ex = blockstore_exists(hash);
    printf("[TEST] blockstore_exists(%s) = %d\n", hash, ex);

    printf("[TEST] getting block \"%s\"…\n", hash);
    uint8_t* out = NULL;
    size_t out_len = 0;

    if (blockstore_get(hash, &out, &out_len) < 0) {
        printf("[TEST] blockstore_get failed\n");
        return 1;
    }

    printf("[TEST] read length = %zu\n", out_len);
    printf("[TEST] read data   = \"%.*s\"\n", (int)out_len, out);

    if (out_len != strlen(text) || memcmp(out, text, out_len) != 0) {
        printf("[TEST] MISMATCH between written and read data!\n");
        free(out);
        return 1;
    }

    free(out);
    printf("[TEST] blockstore test OK ✅\n");

    printf("[TEST] blocks are stored under: %s\n", ENGINE_BLOCKS_DIR);
    return 0;
}
