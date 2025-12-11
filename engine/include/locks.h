// include/locks.h
#pragma once

#include <pthread.h>

// Global reader/writer locks for shared engine resources.
extern pthread_rwlock_t g_blockstore_lock;
extern pthread_rwlock_t g_manifest_lock;

// Initialize all global locks used by the engine.
// Returns 0 on success, -1 on failure.
int locks_init(void);

// Destroy all global locks.
// Safe to call once during engine shutdown.
void locks_shutdown(void);
