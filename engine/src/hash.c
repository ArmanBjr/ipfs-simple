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


#include "blake3.h"


// Fake/educational multicodec code for "manifest" (must be < 0x80 to be a 1-byte varint)
#define CID_CODEC_MANIFEST  0x71  


// void hash_selftest_blake3(void) {
//     const uint8_t msg[] = "test";
//     uint8_t out[32];

//     blake3_hasher hasher;
//     blake3_hasher_init(&hasher);
//     blake3_hasher_update(&hasher, msg, sizeof(msg) - 1);
//     blake3_hasher_finalize(&hasher, out, 32);
//     (void)out;
// }


int hash_compute(hash_algo_t algo, const uint8_t* data, size_t len, hash_result_t* out) {
    if (!data || !out) {
        log_error("[HASH] hash_compute: NULL argument");
        return -1;
    }


    if (algo != HASH_ALGO_BLAKE3) {
        log_error("[HASH] unsupported algo=%d, using BLAKE3 instead", (int)algo);
        algo = HASH_ALGO_BLAKE3;
    }

    const size_t DIGEST_LEN = 32;  

    uint8_t* buf = (uint8_t*)malloc(DIGEST_LEN);
    if (!buf) {
        log_error("[HASH] malloc failed in hash_compute");
        return -1;
    }

    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, data, len);
    blake3_hasher_finalize(&hasher, buf, DIGEST_LEN);

    out->algo       = algo;
    out->digest     = buf;
    out->digest_len = DIGEST_LEN;

    return 0;
}



// RFC 4648 base32 alphabet, lower-case for CIDv1 ("b" multibase)
static const char BASE32_ALPHABET[] = "abcdefghijklmnopqrstuvwxyz234567";

static char* base32_encode(const uint8_t* data, size_t len) {
    if (!data || len == 0) {
        char* out = (char*)malloc(1);
        if (out) out[0] = '\0';
        return out;
    }

    
    size_t out_len = (len * 8 + 4) / 5;  
    char* out = (char*)malloc(out_len + 1);
    if (!out) return NULL;

    size_t bit_buffer = 0;
    int bit_count = 0;
    size_t out_pos = 0;

    for (size_t i = 0; i < len; i++) {
        bit_buffer = (bit_buffer << 8) | data[i];
        bit_count += 8;

        while (bit_count >= 5) {
            int index = (bit_buffer >> (bit_count - 5)) & 0x1F;
            bit_count -= 5;
            out[out_pos++] = BASE32_ALPHABET[index];
        }
    }

    if (bit_count > 0) {
        int index = (bit_buffer << (5 - bit_count)) & 0x1F;
        out[out_pos++] = BASE32_ALPHABET[index];
    }

    out[out_pos] = '\0';
    return out;
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

    // 1) multihash: [code][len][digest...]
    size_t mh_len = 2 + h->digest_len;
    uint8_t* mh = (uint8_t*)malloc(mh_len);
    if (!mh) {
        log_error("[HASH] malloc failed for multihash buffer");
        return -1;
    }

    mh[0] = 0x1E;                    // کد رسمی BLAKE3-256 در multihash
    mh[1] = (uint8_t)h->digest_len;  // باید 32 باشد
    memcpy(mh + 2, h->digest, h->digest_len);

    // 2) payload = multicodec(manifest) || multihash
    size_t payload_len = 1 + mh_len; // چون multicodec اینجا 1 بایتی است
    uint8_t* payload = (uint8_t*)malloc(payload_len);
    if (!payload) {
        log_error("[HASH] malloc failed for payload buffer");
        free(mh);
        return -1;
    }

    payload[0] = CID_CODEC_MANIFEST;  // multicodec(manifest)
    memcpy(payload + 1, mh, mh_len);
    free(mh);

    // 3) base32(payload)  (بدون prefix)
    char* b32 = base32_encode(payload, payload_len);
    free(payload);
    if (!b32) {
        log_error("[HASH] base32_encode failed");
        return -1;
    }

    // 4) multibase prefix 'b' + base32 → CID string
    size_t b32_len = strlen(b32);
    char* cid = (char*)malloc(b32_len + 2); // 'b' + ... + '\0'
    if (!cid) {
        log_error("[HASH] malloc failed for CID string");
        free(b32);
        return -1;
    }

    cid[0] = 'b';                 // multibase(base32, lower-case)
    memcpy(cid + 1, b32, b32_len);
    cid[b32_len + 1] = '\0';
    free(b32);

    *out_str = cid;
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

