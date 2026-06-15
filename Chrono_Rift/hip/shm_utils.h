#pragma once
#include "shared_types.h"
#include <sys/ipc.h>
#include <sys/shm.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>

// Creates or attaches to shared memory segment
// create=true  → IPC_CREAT | IPC_EXCL
// create=false → attach only
inline GameState* shm_attach(bool create) {
    int flags = 0666;
    if (create) flags |= IPC_CREAT | IPC_EXCL;

    int shmid = shmget(SHM_KEY, SHM_SIZE, flags);
    if (shmid == -1) {
        // If already exists and we're creating, try attaching
        if (create && errno == EEXIST) {
            shmid = shmget(SHM_KEY, SHM_SIZE, 0666);
        }
        if (shmid == -1) {
            perror("shmget");
            return nullptr;
        }
    }
    void* ptr = shmat(shmid, nullptr, 0);
    if (ptr == (void*)-1) {
        perror("shmat");
        return nullptr;
    }
    return static_cast<GameState*>(ptr);
}

inline void shm_detach(GameState* gs) {
    if (gs) shmdt(gs);
}

inline void shm_destroy() {
    int shmid = shmget(SHM_KEY, SHM_SIZE, 0666);
    if (shmid != -1) shmctl(shmid, IPC_RMID, nullptr);
}

// Initialize all mutexes/conds/sems as process-shared
inline bool shm_init_sync(GameState* gs) {
    pthread_mutexattr_t mattr;
    pthread_condattr_t  cattr;

    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);

    pthread_condattr_init(&cattr);
    pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED);

    if (pthread_mutex_init(&gs->state_lock,  &mattr) != 0) return false;
    if (pthread_mutex_init(&gs->log_lock,    &mattr) != 0) return false;
    if (pthread_mutex_init(&gs->action_lock, &mattr) != 0) return false;
    if (pthread_cond_init(&gs->action_ready_cond, &cattr) != 0) return false;
    if (pthread_cond_init(&gs->turn_assigned_cond,&cattr) != 0) return false;

    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (pthread_mutex_init(&gs->input_lock[i], &mattr) != 0) return false;
        if (pthread_cond_init(&gs->input_cond[i],  &cattr) != 0) return false;
        if (pthread_mutex_init(&gs->gui_lock[i], &mattr) != 0) return false;
        if (pthread_cond_init(&gs->gui_cond[i],  &cattr) != 0) return false;
        gs->gui_input[i].ready = false;
    }
    gs->gui_mode = false;
    gs->gui_viewing_player = 0;

    // Resource table lock
    if (pthread_mutex_init(&gs->resource_table.lock, &mattr) != 0) return false;

    // Render semaphore
    sem_init(&gs->render_sem, 1, 0); // process-shared

    pthread_mutexattr_destroy(&mattr);
    pthread_condattr_destroy(&cattr);
    return true;
}

inline void shm_destroy_sync(GameState* gs) {
    pthread_mutex_destroy(&gs->state_lock);
    pthread_mutex_destroy(&gs->log_lock);
    pthread_mutex_destroy(&gs->action_lock);
    pthread_cond_destroy(&gs->action_ready_cond);
    pthread_cond_destroy(&gs->turn_assigned_cond);
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        pthread_mutex_destroy(&gs->input_lock[i]);
        pthread_cond_destroy(&gs->input_cond[i]);
        pthread_mutex_destroy(&gs->gui_lock[i]);
        pthread_cond_destroy(&gs->gui_cond[i]);
    }
    pthread_mutex_destroy(&gs->resource_table.lock);
    sem_destroy(&gs->render_sem);
}
