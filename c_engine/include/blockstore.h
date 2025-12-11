// blockstore.h
#pragma once

#include <stddef.h>
#include <stdint.h>

// Initialize blockstore directory; if base_dir == NULL use ENGINE_BLOCKS_DIR
int blockstore_init(const char* base_dir);

// Store a block under its multihash base32 name
// If the block already exists, implementation may treat it as success
int blockstore_put(const char* hash_str, const uint8_t* data, size_t len);

// Read a block into a malloc'ed buffer (caller must free)
// Returns 0 on success, negative on error
int blockstore_get(const char* hash_str, uint8_t** out_data, size_t* out_len);

// Check whether a block exists
// return: 1 = exists, 0 = does not exist, <0 = error
int blockstore_exists(const char* hash_str);

// Optional: remove block (for GC or cleanup)
int blockstore_remove(const char* hash_str);

// include/blockstore.h
int blockstore_make_path(const char* hash_str, char* out_path);

