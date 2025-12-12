#include <stdio.h>
#include <pthread.h>

#include "locks.h"

pthread_rwlock_t g_blockstore_lock;
pthread_rwlock_t g_manifest_lock;

int locks_init(void) {
    int rc;

    rc = pthread_rwlock_init(&g_blockstore_lock, NULL);
    if (rc != 0) {
        fprintf(stderr,
                "[LOCKS] ERROR: pthread_rwlock_init(g_blockstore_lock) failed (rc=%d)\n",
                rc);
        return -1;
    }

    rc = pthread_rwlock_init(&g_manifest_lock, NULL);
    if (rc != 0) {
        fprintf(stderr,
                "[LOCKS] ERROR: pthread_rwlock_init(g_manifest_lock) failed (rc=%d)\n",
                rc);
        pthread_rwlock_destroy(&g_blockstore_lock);
        return -1;
    }

    fprintf(stderr, "[LOCKS] locks_init: rwlocks initialized\n");
    return 0;
}

void locks_shutdown(void) {
    int rc;

    rc = pthread_rwlock_destroy(&g_blockstore_lock);
    if (rc != 0) {
        fprintf(stderr,
                "[LOCKS] WARNING: pthread_rwlock_destroy(g_blockstore_lock) failed (rc=%d)\n",
                rc);
    }

    rc = pthread_rwlock_destroy(&g_manifest_lock);
    if (rc != 0) {
        fprintf(stderr,
                "[LOCKS] WARNING: pthread_rwlock_destroy(g_manifest_lock) failed (rc=%d)\n",
                rc);
    }

    fprintf(stderr, "[LOCKS] locks_shutdown: rwlocks destroyed\n");
}
