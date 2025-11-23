#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "hash.h"

int main(void) {
    printf("[TEST] hash module test start\n");

    const char* msg = "hello";
    size_t msg_len = 5;

    hash_result_t h;
    memset(&h, 0, sizeof(h));

    // Compute fake hash
    if (hash_compute(HASH_ALGO_BLAKE3,
                     (const uint8_t*)msg,
                     msg_len,
                     &h) < 0) {
        printf("[TEST] hash_compute FAILED\n");
        return 1;
    }

    // Convert to pseudo-base32 (hex string)
    char* mh = NULL;
    if (hash_to_multihash_b32(&h, &mh) < 0) {
        printf("[TEST] hash_to_multihash_b32 FAILED\n");
        hash_result_free(&h);
        return 1;
    }

    printf("[TEST] message = \"%s\"\n", msg);
    printf("[TEST] digest_len = %zu\n", h.digest_len);
    printf("[TEST] fake multihash (hex) = %s\n", mh);

    // Cleanup
    free(mh);
    hash_result_free(&h);

    printf("[TEST] hash module OK ✔️\n");
    return 0;
}
