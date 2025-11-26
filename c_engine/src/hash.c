// src/hash.c
//
// Phase 4 - Step 1:
// Only provide an empty skeleton for the hashing module.
// The actual logic will be implemented in next steps (fake hash first,
// real BLAKE3 later).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <strings.h>  // for strcasecmp on POSIX

#include "hash.h"
#include "util.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "hash.h"

int hash_compute(hash_algo_t algo, const uint8_t* data, size_t len, hash_result_t* out) {
    if (!data || !out) return -1;

    (void)algo; 

    out->algo = algo;
    out->digest_len = 8;
    out->digest = (uint8_t*)malloc(8);
    if (!out->digest) return -1;

// FNV-1a 64-bit fake hash 
    uint64_t h = 1469598103934665603ULL;      // offset basis
    const uint64_t prime = 1099511628211ULL;  // FNV prime

    for (size_t i = 0; i < len; ++i) {
        h ^= (uint64_t)data[i];
        h *= prime;
    }

    for (int i = 0; i < 8; ++i) {
        out->digest[7 - i] = (uint8_t)((h >> (i * 8)) & 0xFF);
    }

    return 0;
}




/**
 * Convert a hash_result_t into a "multihash + base32-like" string.
 *
 * NOTE:
 *   This is a FAKE / SIMPLIFIED encoding for phase 4.
 *   We build a simple multihash layout:
 *
 *      [ 0 ] = 0x01                 (fake multihash algorithm code)
 *      [ 1 ] = digest_len           (length of digest in bytes)
 *      [ 2.. ] = raw digest bytes
 *
 *   Then we encode the whole multihash buffer as an uppercase hex string.
 *   This is NOT real RFC4648 base32, but good enough as a stable identifier
 *   for integration and testing. Later we can replace it with real multihash
 *   + base32 without changing the API.
 *
 * On success:
 *   *out_str will point to a malloc'ed C-string that must be freed by caller.
 *
 * Returns:
 *   0  on success
 *  -1  on error
 */
 int hash_to_multihash_b32(const hash_result_t* h, char** out_str) {
    if (!h || !out_str) {
        log_error("[HASH] hash_to_multihash_b32: NULL argument");
        return -1;
    }
    if (!h->digest || h->digest_len == 0) {
        log_error("[HASH] hash_to_multihash_b32: empty digest");
        return -1;
    }

    // Build a simple multihash buffer: [code][length][digest...]
    size_t total_len = 2 + h->digest_len;
    uint8_t* mh = (uint8_t*)malloc(total_len);
    if (!mh) {
        log_error("[HASH] malloc failed for multihash buffer");
        return -1;
    }

    mh[0] = 0x01;                    // fake multihash algorithm code
    mh[1] = (uint8_t)h->digest_len;  // digest length
    memcpy(mh + 2, h->digest, h->digest_len);

    // "Base32-like" encoding: we simply output uppercase hex (2 chars per byte).
    size_t hex_len = total_len * 2;
    char* hex = (char*)malloc(hex_len + 1);
    if (!hex) {
        log_error("[HASH] malloc failed for hex string");
        free(mh);
        return -1;
    }

    for (size_t i = 0; i < total_len; i++) {
        // Each byte becomes two uppercase hex characters.
        sprintf(hex + (i * 2), "%02X", mh[i]);
    }
    hex[hex_len] = '\0';

    free(mh);
    *out_str = hex;
    return 0;
}

/**
 * Free the memory held inside a hash_result_t.
 *
 * This does NOT free the struct itself, only the internal digest buffer.
 * Caller is responsible for freeing the hash_result_t if it was malloc'ed.
 */
 void hash_result_free(hash_result_t* h) {
    if (!h) {
        return;
    }

    if (h->digest) {
        free(h->digest);
        h->digest = NULL;
    }

    h->digest_len = 0;
}


/**
 * Read selected hashing algorithm from environment variable HASH_ALGO.
 *
 * Currently we only support BLAKE3, but this function is written so that
 * other algorithms can be added later.
 *
 * Examples:
 *   export HASH_ALGO=blake3
 *
 * If HASH_ALGO is not set or has an unknown value, we fall back to BLAKE3.
 */
 hash_algo_t hash_algo_from_env(void) {
    const char* env = getenv("HASH_ALGO");
    if (!env || !env[0]) {
        // No environment variable set → default to BLAKE3
        return HASH_ALGO_BLAKE3;
    }

    if (strcasecmp(env, "blake3") == 0) {
        return HASH_ALGO_BLAKE3;
    }

    // Unknown algorithm name; log and fall back
    log_error("[HASH] unknown HASH_ALGO=\"%s\", falling back to blake3", env);
    return HASH_ALGO_BLAKE3;
}

