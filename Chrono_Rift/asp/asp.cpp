/*
 * asp.cpp
 * Automated Strategic Process
 * Each enemy NPC runs in its own dedicated thread.
 * Handles stun mechanic (SIGRTMIN) and communicates via shared memory.
 */

#include "shared_types.h"
#include "shm_utils.h"
#include "inventory.h"
#include "game_log.h"
#include "signals_def.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <cerrno>
#include <ctime>

// ─── Globals ──────────────────────────────────────────────────────────────────
static GameState* gs = nullptr;
static int enemy_count = 2;

// Per-enemy stun state (thread-local via per-thread pthread key or per-index)
static volatile bool enemy_stunned[MAX_ENEMIES] = {};

// ─── NPC AI Decision ─────────────────────────────────────────────────────────
// Uses rand_r with a per-thread seed (passed in) to avoid data races on the
// global rand() state — which caused enemies to always resolve target index 0
// (Hero-1) when multiple threads called rand() concurrently.
static Action compute_npc_action(int eid, unsigned int* rng_seed) {
    Action act = {};
    act.actor_id = eid;
    act.valid    = true;

    Entity& e = gs->entities[eid];

    // If stamina is too low, skip to recover
    if (e.stamina < 10) {
        act.type = ActionType::SKIP;
        return act;
    }

    // 15% chance to skip
    if (rand_r(rng_seed) % 7 == 0) {
        act.type = ActionType::SKIP;
        return act;
    }

    // 20% chance to heal if HP < 50%
    if (e.hp < e.max_hp / 2 && rand_r(rng_seed) % 5 < 2) {
        act.type = ActionType::HEAL;
        return act;
    }

    // 25% chance to try acquiring an unclaimed artifact (resource contention!)
    if (rand_r(rng_seed) % 4 == 0) {
        for (int a = 0; a < 3; ++a) {
            ArtifactEntry& art = gs->resource_table.artifacts[a];
            if (art.exists && art.held_by < 0) {
                act.type = ActionType::ACQUIRE_ARTIFACT;
                act.weapon_id = art.weapon_id;
                return act;
            }
            // If someone else holds an artifact we want, record the wait
            // This can create the deadlock condition!
            if (art.exists && art.held_by >= 0 && art.held_by != eid) {
                // Check if WE hold a different artifact — that's hold-and-wait!
                bool we_hold_artifact = false;
                for (int b = 0; b < 3; ++b) {
                    if (b != a && gs->resource_table.artifacts[b].exists &&
                        gs->resource_table.artifacts[b].held_by == eid) {
                        we_hold_artifact = true;
                        break;
                    }
                }
                if (we_hold_artifact && rand_r(rng_seed) % 3 == 0) {
                    // Try to acquire the held artifact — creates deadlock potential
                    act.type = ActionType::ACQUIRE_ARTIFACT;
                    act.weapon_id = art.weapon_id;
                    return act;
                }
            }
        }
    }

    // Find target: pick a truly random alive player using the thread-local seed.
    // Previously rand() (global state) was called concurrently by all enemy
    // threads without locks, so it was effectively deterministic — the modulo
    // almost always landed on index 0, i.e. Hero-1.  rand_r fixes this.
    int alive_players[MAX_PLAYERS];
    int alive_count = 0;
    for (int i = 0; i < gs->player_count; ++i) {
        if (gs->entities[i].is_alive())
            alive_players[alive_count++] = i;
    }
    int target = -1;
    if (alive_count > 0)
        target = alive_players[rand_r(rng_seed) % alive_count];
    if (target == -1) {
        act.type = ActionType::SKIP;
        return act;
    }

    // 50% chance to use weapon if we have one (weapons are stronger than strikes)
    if (rand_r(rng_seed) % 2 == 0) {
        // Find strongest weapon in inventory
        WeaponID best = WeaponID::NONE;
        int best_dmg = 0;
        for (int i = 0; i < INVENTORY_SLOTS; ++i) {
            WeaponID w = e.inventory.slots[i];
            if (w == WeaponID::NONE) continue;
            // Skip duplicates
            if (i > 0 && e.inventory.slots[i-1] == w) continue;
            const WeaponInfo& wi = getWeaponInfo(w);
            if (wi.damage > best_dmg) {
                best_dmg = wi.damage;
                best = w;
            }
        }
        if (best != WeaponID::NONE && best_dmg > e.damage) {
            act.type = ActionType::USE_WEAPON;
            act.weapon_id = best;
            act.target_id = target;
            return act;
        }
    }

    // Default: normal strike
    act.type      = ActionType::STRIKE;
    act.target_id = target;
    return act;
}

// ─── Stun Signal Handler ──────────────────────────────────────────────────────
// Each enemy thread masks/unmasks this signal
static void handle_stun_signal(int) {
    // The thread receiving this signal will check its entity's stun flag
    // and sleep accordingly. We just mark it here.
}

// ─── Enemy Thread ─────────────────────────────────────────────────────────────
struct EnemyThreadArg {
    int enemy_index; // 0-based within enemy array
    int entity_id;   // gs->entities index (MAX_PLAYERS + enemy_index)
};

static void* enemy_thread_func(void* arg) {
    EnemyThreadArg* earg = (EnemyThreadArg*)arg;
    int eid  = earg->entity_id;
    int eidx = earg->enemy_index;

    // Set up stun signal handler
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_stun_signal;
    sigaction(SIG_STUN, &sa, nullptr);

    // Each thread gets its own independent seed so rand_r calls don't race
    // with other enemy threads.  We XOR the entity id and thread creation time
    // so two enemies that start in the same second still diverge.
    unsigned int rng_seed = (unsigned int)(time(nullptr) ^ ((unsigned long long)eid * 2654435761u));

    while (!gs->game_over) {
        // Wait until it's this enemy's turn
        pthread_mutex_lock(&gs->state_lock);
        while (gs->current_actor_id != eid && !gs->game_over) {
            pthread_cond_wait(&gs->turn_assigned_cond, &gs->state_lock);
        }
        pthread_mutex_unlock(&gs->state_lock);

        if (gs->game_over) break;

        Entity& e = gs->entities[eid];
        if (!e.is_alive()) {
            // Dead entity: skip
            pthread_mutex_lock(&gs->action_lock);
            gs->pending_action = { eid, ActionType::SKIP, -1, WeaponID::NONE, true };
            gs->action_pending = true;
            pthread_cond_signal(&gs->action_ready_cond);
            pthread_mutex_unlock(&gs->action_lock);
            // Wait for arbiter to consume
            pthread_mutex_lock(&gs->state_lock);
            while (gs->current_actor_id == eid && !gs->game_over) {
                pthread_cond_wait(&gs->turn_assigned_cond, &gs->state_lock);
            }
            pthread_mutex_unlock(&gs->state_lock);
            continue;
        }

        // Handle stun
        if (e.stun_active) {
            time_t now = time(nullptr);
            if (now < e.stun_end_time) {
                long remaining = (long)(e.stun_end_time - now);
                game_log(gs, "[STUN] %s is stunned for %ld more seconds!",
                         e.name, remaining);
                sleep((unsigned)(remaining > 0 ? remaining : 1));
            }
            e.stun_active = false;
            e.state = EntityState::ALIVE;
            // Skip turn
            pthread_mutex_lock(&gs->action_lock);
            gs->pending_action = { eid, ActionType::SKIP, -1, WeaponID::NONE, true };
            gs->action_pending = true;
            pthread_cond_signal(&gs->action_ready_cond);
            pthread_mutex_unlock(&gs->action_lock);
            // Wait for arbiter to consume
            pthread_mutex_lock(&gs->state_lock);
            while (gs->current_actor_id == eid && !gs->game_over) {
                pthread_cond_wait(&gs->turn_assigned_cond, &gs->state_lock);
            }
            pthread_mutex_unlock(&gs->state_lock);
            continue;
        }

        // Think for a moment (simulate decision latency)
        struct timespec think = {0, (long)(200 + rand() % 300) * 1000000};
        nanosleep(&think, nullptr);

        // Compute action (pass thread-local seed for thread-safe randomness)
        Action act = compute_npc_action(eid, &rng_seed);

        // Submit
        pthread_mutex_lock(&gs->action_lock);
        gs->pending_action = act;
        gs->action_pending = true;
        pthread_cond_signal(&gs->action_ready_cond);
        pthread_mutex_unlock(&gs->action_lock);

        // Wait until arbiter consumes our action (resets current_actor_id away from us)
        pthread_mutex_lock(&gs->state_lock);
        while (gs->current_actor_id == eid && !gs->game_over) {
            pthread_cond_wait(&gs->turn_assigned_cond, &gs->state_lock);
        }
        pthread_mutex_unlock(&gs->state_lock);
    }

    return nullptr;
}

// ─── SIGTERM handler ──────────────────────────────────────────────────────────
static void handle_sigterm(int) {
    shm_detach(gs);
    exit(0);
}

// ─── SIGSTOP / SIGCONT are handled by kernel (process level suspend/resume) ──

// ─── Main ─────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    if (argc < 2) { fprintf(stderr, "Usage: asp <roll_no> <enemy_count>\n"); return 1; }
    int roll_no = atoi(argv[1]);
    enemy_count = (argc >= 3) ? atoi(argv[2]) : 2;

    gs = shm_attach(false);
    if (!gs) { fprintf(stderr, "[ASP] Failed to attach shared memory\n"); return 1; }

    signal(SIGTERM, handle_sigterm);
    signal(SIGPIPE, SIG_IGN);

    gs->asp_pid = getpid();
    srand((unsigned)roll_no ^ 0xDEADBEEF);

    // Create one thread per enemy
    pthread_t threads[MAX_ENEMIES];
    EnemyThreadArg args[MAX_ENEMIES];

    for (int i = 0; i < enemy_count; ++i) {
        args[i].enemy_index = i;
        args[i].entity_id   = MAX_PLAYERS + i;
        pthread_create(&threads[i], nullptr, enemy_thread_func, &args[i]);
    }

    // Main thread just waits — actual work is in enemy threads
    while (!gs->game_over) {
        sleep(1);
    }

    // Join all threads
    for (int i = 0; i < enemy_count; ++i) {
        pthread_cancel(threads[i]);
        pthread_join(threads[i], nullptr);
    }

    shm_detach(gs);
    return 0;
}
