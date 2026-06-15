/*
 * arbiter.cpp
 * The Game Arbiter — central authority process.
 * Manages global state, scheduling, deadlock detection.
 */

#include "shared_types.h"
#include "shm_utils.h"
#include "inventory.h"
#include "game_log.h"
#include "signals_def.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <pthread.h>
#include <time.h>
#include <cerrno>
#include <algorithm>
#include <vector>

// ─── Globals ──────────────────────────────────────────────────────────────────
static GameState* gs  = nullptr;
static int        roll_number = 0;  // set from argv

// ─── Turn Queue ───────────────────────────────────────────────────────────────
// Build a turn order: hero0, enemy0, hero1, enemy1, hero2, enemy2, ...
// If more enemies than heroes, remaining enemies get appended at the end.
// If more heroes than enemies, remaining heroes get appended.
static std::vector<int> turn_queue;
static int turn_queue_index = 0;

static void build_turn_queue(int player_count, int enemy_count) {
    turn_queue.clear();
    int max_pairs = std::max(player_count, enemy_count);
    for (int i = 0; i < max_pairs; ++i) {
        if (i < player_count)
            turn_queue.push_back(i);                    // hero i
        if (i < enemy_count)
            turn_queue.push_back(MAX_PLAYERS + i);      // enemy i
    }
    turn_queue_index = 0;
}

// Stamina costs per action
static constexpr int STA_STRIKE   = 25;
static constexpr int STA_EXHAUST  = 20;
static constexpr int STA_WEAPON   = 30;
static constexpr int STA_SWAP     = 10;
static constexpr int STA_HEAL     = 15;
static constexpr int STA_ULTIMATE = 50;
static constexpr int STA_REGEN    = 30;  // gained per turn
static constexpr int STA_SKIP_REGEN = 40; // bonus for skipping
static constexpr int STA_MIN_ACT  = 10;  // minimum to act (else forced skip)

// Pick the next alive, non-stunned actor from the turn queue.
// Cycles through the queue, skipping dead/stunned entities.
static int pick_next_actor() {
    if (turn_queue.empty()) return -1;
    int size = (int)turn_queue.size();
    for (int attempt = 0; attempt < size; ++attempt) {
        int eid = turn_queue[turn_queue_index];
        turn_queue_index = (turn_queue_index + 1) % size;
        Entity& e = gs->entities[eid];
        if (!e.is_alive()) continue;
        if (!e.stun_active) {
            // Regenerate stamina each turn (partial, not full reset)
            e.stamina = std::min(e.max_stamina, e.stamina + STA_REGEN);
            return eid;
        }
        // Stunned — check if stun has expired
        time_t now = time(nullptr);
        if (now >= e.stun_end_time) {
            e.stun_active = false;
            e.state       = EntityState::ALIVE;
            game_log(gs, "%s recovers from stun!", e.name);
            e.stamina = std::min(e.max_stamina, e.stamina + STA_REGEN);
            return eid;
        }
        // Still stunned. Skip enemies — but NEVER skip players.
        // Ultimate only stuns enemies; the hero must still get turns while
        // enemies are frozen, otherwise the scheduler stalls for 10 seconds.
        if (e.type == EntityType::PLAYER) {
            e.stamina = std::min(e.max_stamina, e.stamina + STA_REGEN);
            return eid;
        }
        // Enemy still stunned — skip to next queue entry
    }
    return -1;
}

// ─── Forward Declarations ─────────────────────────────────────────────────────
static void* deadlock_monitor_thread(void*);
static void  handle_sigterm(int);
static void  handle_sigchld(int);
static void  handle_sigalrm(int);
static void  apply_action(Action& act);
static void  drop_weapon_on_kill(int enemy_id, int killer_id);
static void  init_entities(int player_count, int enemy_count, unsigned seed);
static void  init_artifacts();
static bool  check_win_loss();

// SIGALRM state for ultimate pause
static volatile sig_atomic_t ultimate_alarm_fired = 0;

// ─── Signal Handlers ──────────────────────────────────────────────────────────
static void handle_sigterm(int) {
    if (gs) {
        gs->quit_requested = true;
        gs->game_over      = true;
    }
}

static void handle_sigchld(int) {
    // Reap zombie children
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) {}
}

static void handle_sigalrm(int) {
    ultimate_alarm_fired = 1;
}

// ─── Entity initialization ────────────────────────────────────────────────────
static void init_entities(int player_count, int enemy_count, unsigned seed) {
    srand(seed);

    gs->player_count = player_count;
    gs->enemy_count  = enemy_count;
    gs->total_enemies_killed = 0;

    // Roll number digits
    int last2 = roll_number % 100;
    int last1 = roll_number % 10;
    int second_last = (roll_number / 10) % 10;

    // Players
    for (int i = 0; i < player_count; ++i) {
        Entity& e = gs->entities[i];
        memset(&e, 0, sizeof(Entity));
        e.id          = i;
        e.type        = EntityType::PLAYER;
        e.state       = EntityState::ALIVE;
        e.player_index = i;
        snprintf(e.name, MAX_NAME_LEN, "Hero-%d", i+1);

        int base_hp   = roll_number + (rand() % 901 + 100); // roll_no + [100,1000]
        e.max_hp      = base_hp;
        e.hp          = base_hp;
        e.damage      = last1 + 10;
        e.speed       = 100 / player_count;
        e.max_stamina = PLAYER_MAX_STAMINA;
        e.stamina     = PLAYER_MAX_STAMINA; // Start with full stamina
        e.stun_active = false;
        e.inventory.clear();
        // Every player starts with a Splinter Stick (weakest weapon)
        e.inventory.placeWeapon(WeaponID::SPLINTER_STICK);
    }

    // Enemies
    for (int i = 0; i < enemy_count; ++i) {
        Entity& e = gs->entities[MAX_PLAYERS + i];
        memset(&e, 0, sizeof(Entity));
        e.id          = MAX_PLAYERS + i;
        e.type        = EntityType::ENEMY;
        e.state       = EntityState::ALIVE;
        e.player_index = -1;
        snprintf(e.name, MAX_NAME_LEN, "Enemy-%d", i+1);

        int base_hp   = last2 + (rand() % 151 + 50); // last2 + [50,200]
        e.max_hp      = base_hp;
        e.hp          = base_hp;
        e.damage      = second_last + 10;
        e.speed       = rand() % 21 + 10;  // [10,30]
        e.max_stamina = ENEMY_MAX_STAMINA;
        e.stamina     = ENEMY_MAX_STAMINA; // Start with full stamina
        e.stun_active = false;
        e.inventory.clear();
    }

    // Artifacts (Solar Core, Lunar Blade, Eclipse Relic) are NOT given to
    // enemies at the start. They enter the game only as drops or via dynamic
    // spawn. Solar Core and Lunar Blade can drop from enemies (50% chance,
    // at most once each for the whole game). Eclipse Relic spawns after 5 kills.

    // Game meta
    gs->current_actor_id  = -1;
    gs->action_pending    = false;
    gs->game_over         = false;
    gs->players_won       = false;
    gs->quit_requested    = false;
    gs->ultimate_pause_active = false;
    gs->eclipse_relic_in_world = false;
    gs->pending_loot.active = false;
    gs->pending_loot.count = 0;

    for (int i = 0; i < MAX_ENTITIES; ++i)
        gs->waiting_for_artifact[i] = -1;
}

// Track whether each unique artifact has already dropped this game.
// (Solar Core index 0, Lunar Blade index 1 — Eclipse Relic spawns separately)
static bool artifact_already_dropped[2] = { false, false };

static void init_artifacts() {
    ResourceTable& rt = gs->resource_table;
    // Solar Core and Lunar Blade exist in the game world but start unclaimed.
    // They drop randomly (at most once each) during gameplay.
    rt.artifacts[0] = { WeaponID::SOLAR_CORE,   -1, true  };
    rt.artifacts[1] = { WeaponID::LUNAR_BLADE,  -1, true  };
    // Eclipse Relic does NOT exist yet — spawns after 5 enemy kills.
    rt.artifacts[2] = { WeaponID::ECLIPSE_RELIC, -1, false };

    // Reset the per-game "already dropped" flags for rare artifacts
    artifact_already_dropped[0] = false;
    artifact_already_dropped[1] = false;
}

// ─── Win/Loss ─────────────────────────────────────────────────────────────────
static bool check_win_loss() {
    if (gs->quit_requested) {
        gs->game_over = true;
        return true;
    }
    // All players dead?
    bool any_player_alive = false;
    for (int i = 0; i < gs->player_count; ++i) {
        if (gs->entities[i].is_alive()) { any_player_alive = true; break; }
    }
    if (!any_player_alive) {
        gs->game_over   = true;
        gs->players_won = false;
        game_log(gs, "=== ALL HEROES FELL. GAME OVER ===");
        return true;
    }
    if (gs->total_enemies_killed >= WIN_KILL_COUNT) {
        gs->game_over   = true;
        gs->players_won = true;
        game_log(gs, "=== VICTORY! 10 ENEMIES SLAIN ===");
        return true;
    }
    return false;
}

// ─── Action Processing ────────────────────────────────────────────────────────
// Helper: update resource table when an artifact changes hands
static void update_artifact_owner(WeaponID wid, int new_owner) {
    for (int i = 0; i < 3; ++i) {
        if (gs->resource_table.artifacts[i].weapon_id == wid) {
            gs->resource_table.artifacts[i].held_by = new_owner;
            return;
        }
    }
}

// Helper: check if a weapon is an artifact (globally unique resource)
static bool is_artifact(WeaponID wid) {
    return wid == WeaponID::SOLAR_CORE ||
           wid == WeaponID::LUNAR_BLADE ||
           wid == WeaponID::ECLIPSE_RELIC;
}

static void drop_weapon_on_kill(int eid, int killer_id) {
    Entity& dead = gs->entities[eid];

    // Clear any dead enemy's inventory silently — enemies carry no loot by
    // default; we generate a random drop below instead.
    dead.inventory.clear();

    // Reset pending loot state
    gs->pending_loot.for_player = killer_id;
    gs->pending_loot.count = 0;
    gs->pending_loot.active = false;

    // Eclipse Relic: spawns dynamically after exactly 5 enemy kills (once ever)
    if (!gs->eclipse_relic_in_world && gs->total_enemies_killed >= 5) {
        gs->eclipse_relic_in_world = true;
        gs->resource_table.artifacts[2].exists  = true;
        gs->resource_table.artifacts[2].held_by = -1;
        game_log(gs, "══════════════════════════════════════════");
        game_log(gs, "!!! ECLIPSE RELIC has materialized !!!");
        game_log(gs, "A new artifact resource enters the system.");
        game_log(gs, "══════════════════════════════════════════");
        // Eclipse Relic goes straight into pending loot (one slot)
        gs->pending_loot.weapons[gs->pending_loot.count++] = WeaponID::ECLIPSE_RELIC;
        update_artifact_owner(WeaponID::ECLIPSE_RELIC, -1);
        gs->pending_loot.active = true;
        game_log(gs, "Eclipse Relic available for %s!", gs->entities[killer_id].name);
        return; // Eclipse Relic IS the drop for this kill
    }

    // Every enemy always drops exactly one weapon (no chance roll).
    // Build the drop pool:
    //   - Normal weapons (can drop multiple times)
    //   - Solar Core / Lunar Blade (each can drop at most ONCE per game)
    WeaponID pool[8];
    int pool_size = 0;

    // Always-available normal weapons
    pool[pool_size++] = WeaponID::IRON_HALBERD;
    pool[pool_size++] = WeaponID::VENOM_DAGGER;
    pool[pool_size++] = WeaponID::THUNDERSTAFF;
    pool[pool_size++] = WeaponID::OBSIDIAN_AXE;
    pool[pool_size++] = WeaponID::FROSTBOW;
    pool[pool_size++] = WeaponID::SPLINTER_STICK;

    // Rare artifacts — included only if they have never dropped before
    if (!artifact_already_dropped[0])  // Solar Core
        pool[pool_size++] = WeaponID::SOLAR_CORE;
    if (!artifact_already_dropped[1])  // Lunar Blade
        pool[pool_size++] = WeaponID::LUNAR_BLADE;

    // Pick one weapon at random from the pool
    WeaponID dropped = pool[rand() % pool_size];

    // If a rare weapon was chosen, mark it as dropped so it won't appear again
    if (dropped == WeaponID::SOLAR_CORE)  artifact_already_dropped[0] = true;
    if (dropped == WeaponID::LUNAR_BLADE) artifact_already_dropped[1] = true;

    game_log(gs, "%s dropped %s!", dead.name, getWeaponInfo(dropped).name);
    if (is_artifact(dropped)) update_artifact_owner(dropped, -1);

    // Place the single dropped weapon into pending loot for the player
    gs->pending_loot.weapons[gs->pending_loot.count++] = dropped;
    gs->pending_loot.active = true;
    game_log(gs, "Loot available for %s! Pick up or decline.",
             gs->entities[killer_id].name);
}

static void apply_action(Action& act) {
    Entity& actor = gs->entities[act.actor_id];

    // Force skip if stamina is too low for any action except skip
    if (act.type != ActionType::SKIP && actor.stamina < STA_MIN_ACT) {
        game_log(gs, "%s is too exhausted to act! (STA: %d/%d)",
                 actor.name, actor.stamina, actor.max_stamina);
        actor.stamina = std::min(actor.max_stamina, actor.stamina + STA_SKIP_REGEN);
        game_log(gs, "%s rests — stamina restored to %d/%d.",
                 actor.name, actor.stamina, actor.max_stamina);
        return;
    }

    switch (act.type) {
        case ActionType::STRIKE: {
            Entity& target = gs->entities[act.target_id];
            int dmg = actor.damage;
            target.hp -= dmg;
            game_log(gs, "%s STRIKES %s for %d damage! (HP: %d/%d)",
                     actor.name, target.name, dmg, target.hp < 0 ? 0 : target.hp, target.max_hp);
            actor.stamina -= STA_STRIKE;
            if (actor.stamina < 0) actor.stamina = 0;
            if (target.hp <= 0) {
                target.hp    = 0;
                target.state = EntityState::DEAD;
                game_log(gs, "%s has been defeated!", target.name);
                if (target.type == EntityType::ENEMY) {
                    gs->total_enemies_killed++;
                    drop_weapon_on_kill(target.id, act.actor_id);
                }
            }
            break;
        }
        case ActionType::EXHAUST: {
            Entity& target = gs->entities[act.target_id];
            int drain = actor.damage + 15; // base damage + bonus drain
            target.stamina -= drain;
            if (target.stamina < 0) target.stamina = 0;
            game_log(gs, "%s EXHAUSTS %s, draining %d stamina! (STA: %d/%d)",
                     actor.name, target.name, drain, target.stamina, target.max_stamina);
            actor.stamina -= STA_EXHAUST;
            if (actor.stamina < 0) actor.stamina = 0;
            break;
        }
        case ActionType::USE_WEAPON: {
            // Verify the actor actually has the weapon
            if (!actor.inventory.hasWeapon(act.weapon_id)) {
                game_log(gs, "%s tried to use %s but doesn't have it!",
                         actor.name, getWeaponInfo(act.weapon_id).name);
                actor.stamina -= STA_WEAPON; if (actor.stamina < 0) actor.stamina = 0;
                break;
            }
            Entity& target = gs->entities[act.target_id];
            const WeaponInfo& wi = getWeaponInfo(act.weapon_id);
            int dmg = wi.damage;
            target.hp -= dmg;
            game_log(gs, "%s uses %s on %s for %d damage!",
                     actor.name, wi.name, target.name, dmg);
            actor.stamina -= STA_WEAPON; if (actor.stamina < 0) actor.stamina = 0;
            if (target.hp <= 0) {
                target.hp    = 0;
                target.state = EntityState::DEAD;
                game_log(gs, "%s has been defeated!", target.name);
                if (target.type == EntityType::ENEMY) {
                    gs->total_enemies_killed++;
                    drop_weapon_on_kill(target.id, act.actor_id);
                }
            }
            break;
        }
        case ActionType::SWAP_IN: {
            bool ok = actor.inventory.swapIn(act.weapon_id);
            if (ok) {
                game_log(gs, "%s swapped in %s from long-term storage.",
                         actor.name, getWeaponInfo(act.weapon_id).name);
                // Update artifact tracking if this is an artifact
                if (is_artifact(act.weapon_id)) {
                    update_artifact_owner(act.weapon_id, act.actor_id);
                    game_log(gs, "[ARTIFACT] %s now actively wields %s.",
                             actor.name, getWeaponInfo(act.weapon_id).name);
                }
            } else {
                game_log(gs, "%s failed to swap in weapon.", actor.name);
            }
            actor.stamina -= STA_SWAP; if (actor.stamina < 0) actor.stamina = 0;
            break;
        }

        case ActionType::HEAL: {
            int heal = actor.max_hp / 10;
            actor.hp += heal;
            if (actor.hp > actor.max_hp) actor.hp = actor.max_hp;
            game_log(gs, "%s HEALS for %d HP! (HP: %d/%d)",
                     actor.name, heal, actor.hp, actor.max_hp);
            actor.stamina -= STA_HEAL; if (actor.stamina < 0) actor.stamina = 0;
            break;
        }
        case ActionType::SKIP: {
            // Skipping restores extra stamina (tactical choice)
            actor.stamina = std::min(actor.max_stamina, actor.stamina + STA_SKIP_REGEN);
            game_log(gs, "%s SKIPS — stamina restored to %d/%d.",
                     actor.name, actor.stamina, actor.max_stamina);
            break;
        }
        case ActionType::ULTIMATE: {
            // Requires Solar Core + Lunar Blade
            if (!actor.inventory.hasWeapon(WeaponID::SOLAR_CORE) ||
                !actor.inventory.hasWeapon(WeaponID::LUNAR_BLADE)) {
                game_log(gs, "%s tried Ultimate but lacks artifacts!", actor.name);
                actor.stamina -= STA_ULTIMATE; if (actor.stamina < 0) actor.stamina = 0;
                break;
            }
            game_log(gs, "*** %s activates ULTIMATE ABILITY! ***", actor.name);
            game_log(gs, "All enemies are STUNNED for %d seconds!", ULTIMATE_PAUSE_SEC);
            gs->ultimate_pause_active = true;
            // NOTE: We do NOT send SIGSTOP to the ASP process.
            // Previously, SIGSTOP suspended the entire enemy process, which
            // caused the arbiter scheduler to stall waiting for an action that
            // could never arrive — freezing the whole game for 10 seconds.
            // The stun flags set below are sufficient: pick_next_actor() skips
            // stunned enemies and still hands turns to heroes, so combat
            // continues normally and heroes can freely attack during the window.
            // Set SIGALRM for 10 seconds — will fire handle_sigalrm to mark end
            ultimate_alarm_fired = 0;
            alarm(ULTIMATE_PAUSE_SEC);
            // Stun all alive enemies (no damage — just freeze them)
            time_t stun_end = time(nullptr) + ULTIMATE_PAUSE_SEC;
            for (int i = MAX_PLAYERS; i < MAX_PLAYERS + gs->enemy_count; ++i) {
                Entity& t = gs->entities[i];
                if (!t.is_alive()) continue;
                t.stun_active   = true;
                t.stun_end_time = stun_end;
                t.state         = EntityState::STUNNED;
                game_log(gs, "  %s is STUNNED for %d seconds!", t.name, ULTIMATE_PAUSE_SEC);
            }
            actor.stamina = 0;
            break;
        }
        case ActionType::QUIT: {
            gs->quit_requested = true;
            gs->game_over      = true;
            break;
        }
    }
}

// ─── Deadlock Monitor Thread ──────────────────────────────────────────────────
static void* deadlock_monitor_thread(void*) {
    while (!gs->game_over) {
        sleep(1);
        pthread_mutex_lock(&gs->resource_table.lock);

        // Build wait-for graph: entity → artifact it waits for
        // Check for circular wait involving Solar Core and Lunar Blade
        int sc_holder = gs->resource_table.artifacts[0].held_by; // Solar Core
        int lb_holder = gs->resource_table.artifacts[1].held_by; // Lunar Blade

        // Check: if sc_holder is waiting for lb, and lb_holder is waiting for sc
        if (sc_holder >= 0 && lb_holder >= 0 && sc_holder != lb_holder) {
            bool sc_waits_for_lb = (gs->waiting_for_artifact[sc_holder] == (int)WeaponID::LUNAR_BLADE);
            bool lb_waits_for_sc = (gs->waiting_for_artifact[lb_holder] == (int)WeaponID::SOLAR_CORE);
            if (sc_waits_for_lb && lb_waits_for_sc) {
                game_log(gs, "[DEADLOCK] Detected between entity %d and %d! Forcing release...",
                         sc_holder, lb_holder);
                // Force the enemy to release (prefer to release enemy's lock)
                int victim = -1;
                if (gs->entities[sc_holder].type == EntityType::ENEMY) victim = sc_holder;
                else if (gs->entities[lb_holder].type == EntityType::ENEMY) victim = lb_holder;
                else victim = lb_holder; // force player if both players

                if (victim == sc_holder) {
                    gs->resource_table.artifacts[0].held_by = -1;
                    gs->entities[victim].inventory.removeWeapon(WeaponID::SOLAR_CORE);
                    gs->waiting_for_artifact[victim] = -1;
                    game_log(gs, "[DEADLOCK] Forced %s to release Solar Core.", gs->entities[victim].name);
                } else {
                    gs->resource_table.artifacts[1].held_by = -1;
                    gs->entities[victim].inventory.removeWeapon(WeaponID::LUNAR_BLADE);
                    gs->waiting_for_artifact[victim] = -1;
                    game_log(gs, "[DEADLOCK] Forced %s to release Lunar Blade.", gs->entities[victim].name);
                }
            }
        }
        pthread_mutex_unlock(&gs->resource_table.lock);
    }
    return nullptr;
}

// ─── Main Scheduler Loop (runs in arbiter main thread) ───────────────────────
static void run_scheduler() {
    while (!gs->game_over) {
        // Handle SIGALRM (ultimate pause expired)
        if (ultimate_alarm_fired) {
            ultimate_alarm_fired = 0;
            gs->ultimate_pause_active = false;
            // Enemies will naturally unfreeze as pick_next_actor() checks
            // stun_end_time and clears stun_active when the timer expires.
            game_log(gs, "Ultimate pause ended. Enemies resume.");
        }

        // Pick next actor from the turn queue
        pthread_mutex_lock(&gs->state_lock);
        int actor_id = pick_next_actor();

        if (actor_id >= 0) {
            gs->current_actor_id = actor_id;
            gs->action_pending   = false;
            game_log(gs, "[TURN] %s's turn!",
                     gs->entities[actor_id].name);
            // Wake up all waiting threads (HIP/ASP) so the right one can act
            pthread_cond_broadcast(&gs->turn_assigned_cond);
        }
        pthread_mutex_unlock(&gs->state_lock);

        if (actor_id < 0) {
            // No one can act (all dead/stunned) — wait a bit and retry
            struct timespec sleep_ts = {0, 200 * 1000000}; // 200ms
            nanosleep(&sleep_ts, nullptr);
            continue;
        }

        // Wait for action from HIP or ASP
        pthread_mutex_lock(&gs->action_lock);

        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);

        // NPC gets 3-second timeout; players get longer
        int timeout_sec = NPC_TIMEOUT_SEC;
        if (gs->entities[actor_id].type == EntityType::PLAYER)
            timeout_sec = 60; // players get 60 seconds
        deadline.tv_sec += timeout_sec;

        int ret = 0;
        while (!gs->action_pending && !gs->game_over && ret != ETIMEDOUT) {
            ret = pthread_cond_timedwait(&gs->action_ready_cond,
                                         &gs->action_lock, &deadline);
        }

        if (!gs->action_pending && !gs->game_over) {
            // NPC timeout: treat as SKIP
            game_log(gs, "[TIMEOUT] %s timed out — auto SKIP",
                     gs->entities[gs->current_actor_id].name);
            Action skip_act;
            skip_act.actor_id  = gs->current_actor_id;
            skip_act.type      = ActionType::SKIP;
            skip_act.valid     = true;
            pthread_mutex_unlock(&gs->action_lock);

            pthread_mutex_lock(&gs->state_lock);
            apply_action(skip_act);
            gs->current_actor_id = -1;
            gs->action_pending   = false;
            pthread_cond_broadcast(&gs->turn_assigned_cond);
            pthread_mutex_unlock(&gs->state_lock);
        } else if (gs->action_pending) {
            Action act = gs->pending_action;
            gs->action_pending   = false;
            pthread_mutex_unlock(&gs->action_lock);

            pthread_mutex_lock(&gs->state_lock);
            gs->current_actor_id = -1;
            apply_action(act);
            pthread_cond_broadcast(&gs->turn_assigned_cond);
            pthread_mutex_unlock(&gs->state_lock);
        } else {
            pthread_mutex_unlock(&gs->action_lock);
        }

        check_win_loss();

        // ── Respawn wave: if all enemies dead but game not won, spawn a new wave ──
        if (!gs->game_over && gs->total_enemies_killed < WIN_KILL_COUNT) {
            bool any_enemy_alive = false;
            for (int i = 0; i < gs->enemy_count; ++i) {
                if (gs->entities[MAX_PLAYERS + i].is_alive()) {
                    any_enemy_alive = true;
                    break;
                }
            }
            if (!any_enemy_alive) {
                pthread_mutex_lock(&gs->state_lock);
                int last2 = roll_number % 100;
                int second_last = (roll_number / 10) % 10;
                game_log(gs, "=== NEW WAVE OF ENEMIES APPEARS! ===");
                for (int i = 0; i < gs->enemy_count; ++i) {
                    Entity& e = gs->entities[MAX_PLAYERS + i];
                    e.state       = EntityState::ALIVE;
                    int base_hp   = last2 + (rand() % 151 + 50);
                    e.max_hp      = base_hp;
                    e.hp          = base_hp;
                    e.damage      = second_last + 10 + (gs->total_enemies_killed / 3);
                    e.max_stamina = ENEMY_MAX_STAMINA;
                    e.stamina     = ENEMY_MAX_STAMINA;
                    e.stun_active = false;
                    e.inventory.clear();
                    game_log(gs, "%s respawns! (HP: %d)", e.name, e.hp);
                }
                // Artifacts are not re-assigned on wave respawn — they remain
                // in the drop pool and can still appear via random drops.
                pthread_mutex_unlock(&gs->state_lock);
            }
        }

        // Small delay between turns so output is readable
        struct timespec sleep_ts = {0, 100 * 1000000}; // 100ms
        nanosleep(&sleep_ts, nullptr);
    }
}

// ─── Main ─────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: arbiter <roll_no> <player_count> <enemy_count> [multiplayer]\n");
        return 1;
    }

    roll_number    = atoi(argv[1]);
    int player_cnt = atoi(argv[2]);
    int enemy_cnt  = atoi(argv[3]);
    bool mp_mode   = (argc >= 5 && atoi(argv[4]) == 1);

    if (player_cnt < 1 || player_cnt > MAX_PLAYERS) player_cnt = 1;
    if (enemy_cnt  < 2 || enemy_cnt  > MAX_ENEMIES) enemy_cnt  = 2;

    // Create shared memory
    shm_destroy(); // clean up any stale segment
    gs = shm_attach(true);
    if (!gs) { fprintf(stderr, "Failed to create shared memory\n"); return 1; }
    memset(gs, 0, sizeof(GameState));

    if (!shm_init_sync(gs)) {
        fprintf(stderr, "Failed to init sync primitives\n");
        shm_destroy();
        return 1;
    }

    gs->arbiter_pid    = getpid();
    gs->multiplayer_mode = mp_mode;
    gs->gui_mode         = true;  // Enable GUI input BEFORE forking children

    // Initialize entities
    init_entities(player_cnt, enemy_cnt, (unsigned)roll_number);
    init_artifacts();

    // Build the turn queue: hero1, enemy1, hero2, enemy2, ...
    build_turn_queue(player_cnt, enemy_cnt);

    // Set up signals
    signal(SIGTERM, handle_sigterm);
    signal(SIGCHLD, handle_sigchld);
    signal(SIGALRM, handle_sigalrm);
    signal(SIGPIPE, SIG_IGN);

    // Start deadlock monitor thread
    pthread_t dl_thread;
    pthread_create(&dl_thread, nullptr, deadlock_monitor_thread, nullptr);
    pthread_detach(dl_thread);

    // Fork HIP (Human Interfacing Process)
    pid_t hip = fork();
    if (hip == 0) {
        // Child: exec hip
        char rn[16], pc[4];
        snprintf(rn, sizeof(rn), "%d", roll_number);
        snprintf(pc, sizeof(pc), "%d", player_cnt);
        execl("./hip", "./hip", rn, pc, mp_mode ? "1" : "0", nullptr);
        perror("execl hip");
        exit(1);
    }
    gs->hip_pid = hip;
    game_log(gs, "[ARBITER] HIP spawned (pid %d)", hip);

    // Fork ASP (Automated Strategic Process)
    pid_t asp = fork();
    if (asp == 0) {
        char rn[16], ec[4];
        snprintf(rn, sizeof(rn), "%d", roll_number);
        snprintf(ec, sizeof(ec), "%d", enemy_cnt);
        execl("./asp", "./asp", rn, ec, nullptr);
        perror("execl asp");
        exit(1);
    }
    gs->asp_pid = asp;
    game_log(gs, "[ARBITER] ASP spawned (pid %d)", asp);

    // Fork Renderer (runs silently — HIP owns the terminal and displays game state)
    pid_t renderer = fork();
    if (renderer == 0) {
        // SFML renderer uses its own window — no stdout redirect needed
        char rn[16];
        snprintf(rn, sizeof(rn), "%d", roll_number);
        execl("./renderer", "./renderer", rn, nullptr);
        perror("execl renderer");
        exit(1);
    }

    game_log(gs, "=== CHRONO RIFT BEGINS ===");
    game_log(gs, "Players: %d | Enemies: %d | Seed: %d", player_cnt, enemy_cnt, roll_number);

    // In multiplayer mode, Player 2 connects from a separate terminal
    if (mp_mode && player_cnt >= 2) {
        int p2_start = player_cnt / 2;
        int p2_count = player_cnt - p2_start;
        game_log(gs, "[MULTIPLAYER] Waiting for Player 2 to connect...");
        game_log(gs, "[MULTIPLAYER] P2 run:  ./multiplayer %d %d", p2_start, p2_count);
        printf("\n  \033[1;33m══════════════════════════════════════════════════\033[0m\n");
        printf("  \033[1;33m  MULTIPLAYER: Player 2 — open a second terminal\033[0m\n");
        printf("  \033[1;33m  and run:  ./multiplayer %d %d\033[0m\n", p2_start, p2_count);
        printf("  \033[1;33m══════════════════════════════════════════════════\033[0m\n\n");
        fflush(stdout);
    }

    // Run main scheduling loop
    run_scheduler();

    // Game over — clean up
    game_log(gs, "=== GAME ENDED ===");
    if (gs->players_won)
        game_log(gs, "Result: VICTORY!");
    else if (gs->quit_requested)
        game_log(gs, "Result: QUIT");
    else
        game_log(gs, "Result: DEFEAT");

    sleep(2); // Let renderer show final state

    // Kill child processes
    if (hip > 0)      kill(hip,      SIGTERM);
    if (asp > 0)      kill(asp,      SIGTERM);
    if (renderer > 0) kill(renderer, SIGTERM);

    sleep(1);
    waitpid(hip,      nullptr, WNOHANG);
    waitpid(asp,      nullptr, WNOHANG);
    waitpid(renderer, nullptr, WNOHANG);

    shm_destroy_sync(gs);
    shm_detach(gs);
    shm_destroy();

    printf("\n[ARBITER] Clean exit.\n");
    return 0;
}
