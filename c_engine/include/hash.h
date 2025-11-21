// hash.h
#pragma once

#include <stddef.h>
#include <stdint.h>

// Supported hashing algorithms
typedef enum {
    HASH_ALGO_BLAKE3 = 1,
    // additional algorithms may be added (e.g., SHA256)
} hash_algo_t;

// Raw hash result
typedef struct {
    hash_algo_t algo;
    uint8_t*    digest;     // raw digest bytes
    size_t      digest_len; // number of bytes in digest
} hash_result_t;

// Compute hash of data
// digest is malloc'ed and must be freed by caller via hash_result_free()
int hash_compute(hash_algo_t algo,
                 const uint8_t* data,
                 size_t len,
                 hash_result_t* out);

// Convert hash_result_t to multihash base32 string
// out_str is malloc'ed and must be freed by caller
int hash_to_multihash_b32(const hash_result_t* h, char** out_str);

// Free internal memory of hash_result (digest)
void hash_result_free(hash_result_t* h);

// Determine hash algorithm from environment variable (e.g., HASH_ALGO=blake3)
// If not set, return ENGINE_DEFAULT_HASH_ALGO
hash_algo_t hash_algo_from_env(void);
