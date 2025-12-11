// manifest.h
#pragma once

#include <stdint.h>
#include <stddef.h>

// Chunk entry inside manifest
typedef struct manifest_chunk {
    uint32_t index;     // chunk index (0..N-1)
    uint32_t size;      // byte size of this chunk
    char*    hash_str;  // multihash base32 of this chunk
} manifest_chunk;

// Manifest describing a complete file
typedef struct manifest {
    uint32_t        version;      // e.g., 1
    char*           filename;     // original filename
    uint64_t        total_size;   // total bytes of file
    uint32_t        chunk_size;   // chunk size used during upload
    char*           hash_algo;    // e.g., "blake3"
    uint32_t        chunk_count;  // number of chunks
    manifest_chunk* chunks;       // array of chunks
    char*           auth_token;   // auth token for this manifest
} manifest;

// Create new manifest
manifest* manifest_create(const char* filename,
                          uint32_t chunk_size,
                          const char* hash_algo);

// Add a chunk to manifest
int manifest_add_chunk(manifest* m,
                       uint32_t index,
                       uint32_t size,
                       const char* hash_str);

// Finalize manifest after upload
void manifest_finalize(manifest* m, uint64_t total_size);

// Serialize to JSON, save to disk, compute CID
// out_cid is malloc'ed
int manifest_save_and_get_cid(const manifest* m, char** out_cid);

// Load manifest from disk by CID
manifest* manifest_load_from_cid(const char* cid);

// Free manifest and all internal data
void manifest_free(manifest* m);

manifest* manifest_load_in_progress(const char* upload_id);

struct manifest* manifest_load_in_progress(const char* upload_id);
int manifest_save_progress(const manifest* m, const char* upload_id);
int manifest_delete(const char* cid);