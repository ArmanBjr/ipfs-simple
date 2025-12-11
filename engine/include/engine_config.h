#pragma once

#define ENGINE_BLOCKS_DIR           "blocks"
#define ENGINE_MANIFEST_DIR         "manifests"
#define ENGINE_MANIFEST_TMP_DIR     "manifests/tmp"

#define ENGINE_CHUNK_SIZE           (256u * 1024u)

#define ENGINE_HASH_ALGO_ENV        "HASH_ALGO"
#define ENGINE_DEFAULT_HASH_ALGO    "blake3"

#define ENGINE_DEFAULT_SOCKET_PATH  "/tmp/engine.sock"

#define ENGINE_MAX_CID_LEN          256
#define ENGINE_MAX_HASH_STR_LEN     256
#define ENGINE_MAX_PATH_LEN         512
