#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <strings.h>

#include "hash.h"
#include "util.h"
#include "blake3.h"

#define CID_CODEC_MANIFEST 0x71

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

int hash_to_multihash_b32(const hash_result_t* h, char** out_str) {
    if (!h || !out_str) {
        log_error("[HASH] hash_to_multihash_b32: NULL argument");
        return -1;
    }
    if (!h->digest || h->digest_len == 0) {
        log_error("[HASH] hash_to_multihash_b32: empty digest");
        return -1;
    }

    size_t mh_len = 2 + h->digest_len;
    uint8_t* mh = (uint8_t*)malloc(mh_len);
    if (!mh) {
        log_error("[HASH] malloc failed for multihash buffer");
        return -1;
    }

    mh[0] = 0x1E;
    mh[1] = (uint8_t)h->digest_len;
    memcpy(mh + 2, h->digest, h->digest_len);

    size_t payload_len = 1 + mh_len;
    uint8_t* payload = (uint8_t*)malloc(payload_len);
    if (!payload) {
        log_error("[HASH] malloc failed for payload buffer");
        free(mh);
        return -1;
    }

    payload[0] = CID_CODEC_MANIFEST;
    memcpy(payload + 1, mh, mh_len);
    free(mh);

    char* b32 = base32_encode(payload, payload_len);
    free(payload);
    if (!b32) {
        log_error("[HASH] base32_encode failed");
        return -1;
    }

    size_t b32_len = strlen(b32);
    char* cid = (char*)malloc(b32_len + 2);
    if (!cid) {
        log_error("[HASH] malloc failed for CID string");
        free(b32);
        return -1;
    }

    cid[0] = 'b';
    memcpy(cid + 1, b32, b32_len);
    cid[b32_len + 1] = '\0';
    free(b32);

    *out_str = cid;
    return 0;
}

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

hash_algo_t hash_algo_from_env(void) {
    const char* env = getenv("HASH_ALGO");
    if (!env || !env[0]) {
        return HASH_ALGO_BLAKE3;
    }

    if (strcasecmp(env, "blake3") == 0) {
        return HASH_ALGO_BLAKE3;
    }

    log_error("[HASH] unknown HASH_ALGO=\"%s\", falling back to blake3", env);
    return HASH_ALGO_BLAKE3;
}

