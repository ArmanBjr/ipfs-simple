#pragma once
#include <stddef.h>
#include <stdint.h>

/**
 * Supported hashing algorithms.
 * For now only BLAKE3 is defined, but the enum allows future extensions
 * (e.g., SHA256).
 */
typedef enum {
    HASH_ALGO_BLAKE3 = 1,
    // Future: SHA256, SHA3, etc.
} hash_algo_t;

/**
 * A generic hash result produced by hash_compute().
 * 
 * - algo       → algorithm used (e.g., BLAKE3)
 * - digest     → raw bytes of the hash output (malloc'ed)
 * - digest_len → number of bytes in digest[]
 */
typedef struct {
    hash_algo_t algo;
    uint8_t*    digest;
    size_t      digest_len;
} hash_result_t;

/**
 * Compute the hash of a raw binary buffer.
 * 
 * Parameters:
 *   algo → the hashing algorithm to use
 *   data → pointer to input bytes
 *   len  → length of input in bytes
 *   out  → output structure (digest will be malloc'ed)
 * 
 * Returns 0 on success, <0 on error.
 */
int hash_compute(hash_algo_t algo, const uint8_t* data, size_t len, hash_result_t* out);

/**
 * Convert a hash_result_t into a Multihash Base32 string.
 *
 * The returned string (out_str) is malloc'ed and must be freed by the caller.
 * 
 * Returns 0 on success, <0 on error.
 */
int hash_to_multihash_b32(const hash_result_t* h, char** out_str);

/**
 * Free the memory allocated inside a hash_result_t structure.
 */
void hash_result_free(hash_result_t* h);

/**
 * Read selected hash algorithm from environment variable HASH_ALGO.
 * If not set or invalid, falls back to HASH_ALGO_BLAKE3.
 */
hash_algo_t hash_algo_from_env(void);
