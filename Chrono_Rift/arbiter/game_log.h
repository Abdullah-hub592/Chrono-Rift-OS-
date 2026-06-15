#pragma once
#include "shared_types.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>

inline void game_log(GameState* gs, const char* fmt, ...) {
    pthread_mutex_lock(&gs->log_lock);

    char buf[ACTION_LOG_LEN];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (gs->log_count < ACTION_LOG_LINES) {
        // Still filling — append at log_count
        strncpy(gs->log_lines[gs->log_count], buf, ACTION_LOG_LEN - 1);
        gs->log_lines[gs->log_count][ACTION_LOG_LEN - 1] = '\0';
        gs->log_count++;
    } else {
        // Full — shift all entries up by one (drop oldest at [0])
        for (int i = 0; i < ACTION_LOG_LINES - 1; ++i)
            memcpy(gs->log_lines[i], gs->log_lines[i + 1], ACTION_LOG_LEN);
        strncpy(gs->log_lines[ACTION_LOG_LINES - 1], buf, ACTION_LOG_LEN - 1);
        gs->log_lines[ACTION_LOG_LINES - 1][ACTION_LOG_LEN - 1] = '\0';
        // log_count stays at ACTION_LOG_LINES
    }

    // Signal render
    sem_post(&gs->render_sem);

    pthread_mutex_unlock(&gs->log_lock);
}

// Print entire log to stdout (for debugging)
inline void dump_log(GameState* gs) {
    pthread_mutex_lock(&gs->log_lock);
    for (int i = 0; i < gs->log_count; ++i) {
        printf("[LOG] %s\n", gs->log_lines[i]);
    }
    pthread_mutex_unlock(&gs->log_lock);
}
