// manifest_test.c
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "engine_config.h"
#include "manifest.h"

int main(void) {
    printf("[TEST] manifest module test start\n");

    // 1) Create manifest
    manifest* m = manifest_create("test.txt", ENGINE_CHUNK_SIZE, "blake3");
    if (!m) {
        printf("[TEST] manifest_create FAILED\n");
        return 1;
    }
    printf("[TEST] created manifest for filename=\"%s\", chunk_size=%u\n",
           m->filename ? m->filename : "(null)",
           m->chunk_size);

    // 2) Add two fake chunks
    if (manifest_add_chunk(m, 0, 10, "HASH_CHUNK_0") < 0) {
        printf("[TEST] manifest_add_chunk (0) FAILED\n");
        manifest_free(m);
        return 1;
    }
    if (manifest_add_chunk(m, 1, 15, "HASH_CHUNK_1") < 0) {
        printf("[TEST] manifest_add_chunk (1) FAILED\n");
        manifest_free(m);
        return 1;
    }

    printf("[TEST] added %u chunks\n", m->chunk_count);

    // 3) Finalize (set total_size & sort)
    manifest_finalize(m, 25);
    printf("[TEST] finalized manifest: total_size=%llu\n",
           (unsigned long long)m->total_size);

    // 4) Save to disk + get CID
    char* cid = NULL;
    if (manifest_save_and_get_cid(m, &cid) < 0 || !cid) {
        printf("[TEST] manifest_save_and_get_cid FAILED\n");
        manifest_free(m);
        return 1;
    }

    printf("[TEST] manifest saved, CID = %s\n", cid);
    printf("[TEST] manifest file should be under: %s/<cid>.json\n",
           ENGINE_MANIFEST_DIR);

    // 5) Load manifest back from CID
    manifest* m2 = manifest_load_from_cid(cid);
    if (!m2) {
        printf("[TEST] manifest_load_from_cid FAILED for cid=%s\n", cid);
        manifest_free(m);
        free(cid);
        return 1;
    }

    printf("\n[TEST] Loaded manifest from disk:\n");
    printf("  filename     = \"%s\"\n", m2->filename ? m2->filename : "(null)");
    printf("  chunk_size   = %u\n",    m2->chunk_size);
    printf("  total_size   = %llu\n",  (unsigned long long)m2->total_size);
    printf("  hash_algo    = \"%s\"\n", m2->hash_algo ? m2->hash_algo : "(null)");
    printf("  chunk_count  = %u\n",    m2->chunk_count);

    for (uint32_t i = 0; i < m2->chunk_count; ++i) {
        manifest_chunk* c = &m2->chunks[i];
        printf("  chunk[%u]: index=%u size=%u hash=\"%s\"\n",
               i, c->index, c->size,
               c->hash_str ? c->hash_str : "(null)");
    }

    // 6) Simple sanity checks
    int ok = 1;
    if (!m2->filename || strcmp(m2->filename, "test.txt") != 0) ok = 0;
    if (m2->chunk_count != 2) ok = 0;
    if (m2->total_size != 25) ok = 0;

    if (ok) {
        printf("\n[TEST] manifest test OK ✅\n");
    } else {
        printf("\n[TEST] manifest test FAILED ❌ (field mismatch)\n");
    }

    // 7) Cleanup
    manifest_free(m);
    manifest_free(m2);
    free(cid);

    return ok ? 0 : 1;
}
