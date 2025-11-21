// locks.h
#pragma once

#include <pthread.h>

// Global locks used by blockstore and manifest modules
extern pthread_rwlock_t g_blockstore_lock;
extern pthread_rwlock_t g_manifest_lock;

// Initialize and destroy locks
int  locks_init(void);
void locks_destroy(void);
