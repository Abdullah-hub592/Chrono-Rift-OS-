/*
 * multiplayer.cpp
 * Bonus: Local Multiplayer Mode
 * A second Human Interfacing Process for a second human player.
 *
 * Player 2 runs this in a SEPARATE terminal (e.g. a second docker exec session).
 * It attaches to the same shared memory as the main game and controls
 * the second half of the hero roster via its own terminal stdin/stdout.
 *
 * Usage:
 *   Terminal 1:  ./chrono_rift          (starts game with mp_mode=1)
 *   Terminal 2:  ./multiplayer <start_player_id> <player_count>
 *
 * Example (2 heroes, P2 controls Hero-2):
 *   Terminal 2:  ./multiplayer 1 1
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

static GameState* gs = nullptr;

static void handle_sigterm(int) {
    if (gs) shm_detach(gs);
    exit(0);
}

// Player 2 controls a subset of the player roster
struct P2ThreadArg {
    int entity_id;
    int player_index;
};

// Mutex to serialize stdin across Player 2's threads
static pthread_mutex_t stdin_lock = PTHREAD_MUTEX_INITIALIZER;

// ─── Display game state summary ──────────────────────────────────────────────
static void show_game_state() {
    printf("\n");
    printf("\033[1;33m╔══════════════════════════════════════════════════════════╗\033[0m\n");
    printf("\033[1;33m║          C H R O N O   R I F T  —  PLAYER 2            ║\033[0m\n");
    printf("\033[1;33m╚══════════════════════════════════════════════════════════╝\033[0m\n");

    printf("\033[36m  Enemies Slain: %d/%d\033[0m\n\n", gs->total_enemies_killed, WIN_KILL_COUNT);

    // Heroes
    printf("\033[1;32m  ── HEROES ──\033[0m\n");
    for (int i = 0; i < gs->player_count; ++i) {
        const Entity& e = gs->entities[i];
        bool cur = (gs->current_actor_id == i);
        if (cur) printf("\033[1;36m>> \033[0m");
        else     printf("   ");

        if (e.state == EntityState::DEAD) {
            printf("\033[1;31m%-16s  *** DEFEATED ***\033[0m\n", e.name);
        } else {
            const char* hpc = "\033[32m";
            int hp_pct = (e.hp * 100) / (e.max_hp > 0 ? e.max_hp : 1);
            if (hp_pct < 50) hpc = "\033[33m";
            if (hp_pct < 25) hpc = "\033[31m";
            printf("\033[1;32m%-16s\033[0m  HP: %s%4d/%-4d\033[0m  STA: %3d/%-3d  DMG:%-3d",
                   e.name, hpc, e.hp, e.max_hp, e.stamina, e.max_stamina, e.damage);
            if (e.stun_active) printf("  \033[1;33m[STUNNED]\033[0m");
            printf("\n");
        }
    }
    printf("\n");

    // Enemies
    printf("\033[1;31m  ── ENEMIES ──\033[0m\n");
    for (int i = 0; i < gs->enemy_count; ++i) {
        int eid = MAX_PLAYERS + i;
        const Entity& e = gs->entities[eid];
        bool cur = (gs->current_actor_id == eid);
        if (cur) printf("\033[1;36m>> \033[0m");
        else     printf("   ");

        if (e.state == EntityState::DEAD) {
            printf("\033[1;31m%-16s  *** DEFEATED ***\033[0m\n", e.name);
        } else {
            const char* hpc = "\033[32m";
            int hp_pct = (e.hp * 100) / (e.max_hp > 0 ? e.max_hp : 1);
            if (hp_pct < 50) hpc = "\033[33m";
            if (hp_pct < 25) hpc = "\033[31m";
            printf("\033[1;31m%-16s\033[0m  HP: %s%4d/%-4d\033[0m  STA: %3d/%-3d  DMG:%-3d",
                   e.name, hpc, e.hp, e.max_hp, e.stamina, e.max_stamina, e.damage);
            if (e.stun_active) printf("  \033[1;33m[STUNNED]\033[0m");
            printf("\n");
        }
    }
    printf("\n");

    // Recent action log (last 5)
    printf("\033[1;36m  ── RECENT ACTIONS ──\033[0m\n");
    pthread_mutex_lock(&gs->log_lock);
    int show = gs->log_count < 5 ? gs->log_count : 5;
    int start = gs->log_count - show;
    for (int i = 0; i < show; ++i) {
        int idx = start + i;
        if (idx >= 0 && idx < ACTION_LOG_LINES)
            printf("   \033[37m%s\033[0m\n", gs->log_lines[idx]);
    }
    pthread_mutex_unlock(&gs->log_lock);
    printf("\n");

    // Artifact Status
    printf("\033[1;35m  \u2500\u2500 ARTIFACTS \u2500\u2500\033[0m\n");
    const char* art_names[] = { "Solar Core", "Lunar Blade", "Eclipse Relic" };
    for (int a = 0; a < 3; ++a) {
        const ArtifactEntry& art = gs->resource_table.artifacts[a];
        if (!art.exists) {
            printf("   \033[90m%-14s  (not yet in world)\033[0m\n", art_names[a]);
        } else if (art.held_by < 0) {
            printf("   \033[1;33m%-14s  \u2605 UNCLAIMED \u2605\033[0m\n", art_names[a]);
        } else {
            printf("   \033[35m%-14s  held by %s\033[0m\n",
                   art_names[a], gs->entities[art.held_by].name);
        }
    }
    printf("\n");

    fflush(stdout);
}

// ─── Show action menu ────────────────────────────────────────────────────────
static void show_menu(const Entity& e) {
    printf("\033[1;33m═══════════════════════════════════════════════════\033[0m\n");
    printf("\033[1;33m  %s's Turn (P2)  |  HP:%d/%d  DMG:%d  STA:%d/%d\033[0m\n",
           e.name, e.hp, e.max_hp, e.damage, e.stamina, e.max_stamina);
    printf("\033[1;33m═══════════════════════════════════════════════════\033[0m\n");
    printf("  \033[1;37m[1]\033[0m Strike       - Attack an enemy (DMG: %d)\n", e.damage);
    printf("  \033[1;37m[2]\033[0m Exhaust      - Drain enemy stamina\n");
    printf("  \033[1;37m[3]\033[0m Use Weapon   - Attack with a weapon\n");
    printf("  \033[1;37m[4]\033[0m Swap In      - Bring weapon from LTS\n");
    printf("  \033[1;37m[5]\033[0m Heal         - Restore 10%% max HP\n");
    printf("  \033[1;37m[6]\033[0m Skip         - Skip your turn\n");
    if (e.inventory.hasWeapon(WeaponID::SOLAR_CORE) &&
        e.inventory.hasWeapon(WeaponID::LUNAR_BLADE)) {
        printf("  \033[1;33m[7]\033[0m ULTIMATE     - \033[1;33mBoth artifacts ready!\033[0m\n");
    }
    bool has_unclaimed = false;
    for (int a = 0; a < 3; ++a) {
        if (gs->resource_table.artifacts[a].exists &&
            gs->resource_table.artifacts[a].held_by < 0) {
            has_unclaimed = true;
            break;
        }
    }
    if (has_unclaimed) {
        printf("  \033[1;35m[8]\033[0m Acquire      - Pick up unclaimed artifact\n");
    }
    printf("  \033[1;31m[q]\033[0m Quit\n");
    printf("\033[1;33m> \033[0m");
    fflush(stdout);
}


// ─── Show alive enemies for target selection ─────────────────────────────────
static int show_targets_and_pick() {
    printf("\n  \033[1;36mSelect target:\033[0m\n");
    for (int i = MAX_PLAYERS; i < MAX_PLAYERS + gs->enemy_count; ++i) {
        if (!gs->entities[i].is_alive()) continue;
        printf("    \033[1;37m[%d]\033[0m %s  HP:%d/%d\n",
               i, gs->entities[i].name, gs->entities[i].hp, gs->entities[i].max_hp);
    }
    printf("  \033[1;33m> Target ID: \033[0m");
    fflush(stdout);

    int target = -1;
    if (scanf("%d", &target) != 1) {
        int c; while ((c = getchar()) != '\n' && c != EOF);
        return -1;
    }
    return target;
}

// ─── Show weapons in inventory ───────────────────────────────────────────────
static int show_weapons_and_pick(const Entity& e) {
    printf("\n  \033[1;36mSelect weapon from inventory:\033[0m\n");
    bool has_any = false;
    for (int i = 0; i < INVENTORY_SLOTS; ++i) {
        WeaponID w = e.inventory.slots[i];
        if (w == WeaponID::NONE) continue;
        if (i > 0 && e.inventory.slots[i-1] == w) continue;
        const WeaponInfo& wi = getWeaponInfo(w);
        printf("    \033[1;37m[%d]\033[0m %s (DMG:%d)\n", (int)w, wi.name, wi.damage);
        has_any = true;
    }
    if (!has_any) {
        printf("    (No weapons in inventory)\n");
        return 0;
    }
    printf("  \033[1;33m> Weapon ID: \033[0m");
    fflush(stdout);

    int wid = 0;
    if (scanf("%d", &wid) != 1) {
        int c; while ((c = getchar()) != '\n' && c != EOF);
        return 0;
    }
    return wid;
}

// ─── Show LTS weapons ───────────────────────────────────────────────────────
static int show_lts_and_pick(const Entity& e) {
    printf("\n  \033[1;36mSelect weapon from Long-Term Storage:\033[0m\n");
    if (e.inventory.lts_count == 0) {
        printf("    (LTS is empty)\n");
        return 0;
    }
    for (int i = 0; i < e.inventory.lts_count; ++i) {
        const WeaponInfo& wi = getWeaponInfo(e.inventory.lts[i]);
        printf("    \033[1;37m[%d]\033[0m %s\n", (int)e.inventory.lts[i], wi.name);
    }
    printf("  \033[1;33m> Weapon ID: \033[0m");
    fflush(stdout);

    int wid = 0;
    if (scanf("%d", &wid) != 1) {
        int c; while ((c = getchar()) != '\n' && c != EOF);
        return 0;
    }
    return wid;
}

// ─── Get player action interactively from stdin ──────────────────────────────
static Action get_player_action(int eid) {
    Action act = {};
    act.actor_id = eid;
    act.valid    = true;

    Entity& e = gs->entities[eid];

    // Lock stdin so only one P2 thread reads at a time
    pthread_mutex_lock(&stdin_lock);

    show_game_state();
    show_menu(e);

    char choice[16] = {};
    if (scanf(" %15s", choice) != 1) {
        act.type = ActionType::SKIP;
        pthread_mutex_unlock(&stdin_lock);
        return act;
    }

    char c = choice[0];

    if (c == 'q' || c == 'Q') {
        act.type = ActionType::QUIT;
        pthread_mutex_unlock(&stdin_lock);
        return act;
    }

    switch (c) {
    case '1': {
        act.type = ActionType::STRIKE;
        act.target_id = show_targets_and_pick();
        break;
    }
    case '2': {
        act.type = ActionType::EXHAUST;
        act.target_id = show_targets_and_pick();
        break;
    }
    case '3': {
        act.type = ActionType::USE_WEAPON;
        int wid = show_weapons_and_pick(e);
        act.weapon_id = (WeaponID)wid;
        if (act.weapon_id == WeaponID::NONE) {
            act.type = ActionType::SKIP;
        } else {
            act.target_id = show_targets_and_pick();
        }
        break;
    }
    case '4': {
        act.type = ActionType::SWAP_IN;
        int wid = show_lts_and_pick(e);
        act.weapon_id = (WeaponID)wid;
        if (act.weapon_id == WeaponID::NONE) {
            act.type = ActionType::SKIP;
        }
        break;
    }
    case '5':
        act.type = ActionType::HEAL;
        break;
    case '6':
        act.type = ActionType::SKIP;
        break;
    case '7':
        act.type = ActionType::ULTIMATE;
        break;
    case '8': { // Acquire Artifact
        act.type = ActionType::ACQUIRE_ARTIFACT;
        printf("\n  \033[1;35mSelect artifact to acquire:\033[0m\n");
        for (int a = 0; a < 3; ++a) {
            const ArtifactEntry& art = gs->resource_table.artifacts[a];
            if (!art.exists) continue;
            const char* status = "";
            if (art.held_by < 0)
                status = "\033[1;33m[UNCLAIMED]\033[0m";
            else
                status = gs->entities[art.held_by].name;
            printf("    \033[1;37m[%d]\033[0m %s  %s\n",
                   (int)art.weapon_id, getWeaponInfo(art.weapon_id).name, status);
        }
        printf("  \033[1;33m> Artifact ID: \033[0m");
        fflush(stdout);
        int aid = 0;
        if (scanf("%d", &aid) != 1) {
            int ch; while ((ch = getchar()) != '\n' && ch != EOF);
            act.type = ActionType::SKIP;
        } else {
            act.weapon_id = (WeaponID)aid;
        }
        break;
    }
    default:
        act.type = ActionType::SKIP;
        break;
    }

    pthread_mutex_unlock(&stdin_lock);
    return act;
}

// ─── Validate target ─────────────────────────────────────────────────────────
static void validate_target(Action& act) {
    if (act.type != ActionType::STRIKE &&
        act.type != ActionType::EXHAUST &&
        act.type != ActionType::USE_WEAPON) return;

    if (act.target_id >= MAX_PLAYERS &&
        act.target_id < MAX_PLAYERS + gs->enemy_count &&
        gs->entities[act.target_id].is_alive()) return;

    act.target_id = -1;
    for (int i = MAX_PLAYERS; i < MAX_PLAYERS + gs->enemy_count; ++i) {
        if (gs->entities[i].is_alive()) { act.target_id = i; break; }
    }
    if (act.target_id == -1) act.type = ActionType::SKIP;
}

// ─── Player 2 Thread ─────────────────────────────────────────────────────────
static void* p2_thread_func(void* arg) {
    P2ThreadArg* parg = (P2ThreadArg*)arg;
    int eid  = parg->entity_id;
    int pidx = parg->player_index;

    while (!gs->game_over) {
        // Wait until it's this player's turn
        pthread_mutex_lock(&gs->state_lock);
        while (gs->current_actor_id != eid && !gs->game_over) {
            pthread_cond_wait(&gs->turn_assigned_cond, &gs->state_lock);
        }
        pthread_mutex_unlock(&gs->state_lock);

        if (gs->game_over) break;

        Entity& e = gs->entities[eid];
        if (!e.is_alive()) {
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
                sleep((unsigned)(remaining > 0 ? remaining : 1));
            }
            e.stun_active = false;
            e.state = EntityState::ALIVE;
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

        Action act;
        if (gs->gui_mode) {
            // GUI mode: wait for renderer to submit action via shared memory
            pthread_mutex_lock(&gs->gui_lock[eid]);
            while (!gs->gui_input[eid].ready && !gs->game_over) {
                pthread_cond_wait(&gs->gui_cond[eid], &gs->gui_lock[eid]);
            }
            act = gs->gui_input[eid].action;
            gs->gui_input[eid].ready = false;
            pthread_mutex_unlock(&gs->gui_lock[eid]);
        } else {
            // Terminal mode: get action from Player 2's terminal
            act = get_player_action(eid);
        }
        validate_target(act);

        // Submit action to Arbiter
        pthread_mutex_lock(&gs->action_lock);
        gs->pending_action = act;
        gs->action_pending = true;
        pthread_cond_signal(&gs->action_ready_cond);
        pthread_mutex_unlock(&gs->action_lock);

        // Wait until arbiter consumes our action
        pthread_mutex_lock(&gs->state_lock);
        while (gs->current_actor_id == eid && !gs->game_over) {
            pthread_cond_wait(&gs->turn_assigned_cond, &gs->state_lock);
        }
        pthread_mutex_unlock(&gs->state_lock);
    }
    return nullptr;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printf("\n");
        printf("  \033[1;33m╔══════════════════════════════════════════════════╗\033[0m\n");
        printf("  \033[1;33m║     CHRONO RIFT — Player 2 Terminal              ║\033[0m\n");
        printf("  \033[1;33m╚══════════════════════════════════════════════════╝\033[0m\n\n");
        printf("  Usage: ./multiplayer <start_player_id> <player_count>\n\n");
        printf("  Example (2 heroes, P2 controls Hero-2):\n");
        printf("    ./multiplayer 1 1\n\n");
        printf("  Example (4 heroes, P2 controls Hero-3 and Hero-4):\n");
        printf("    ./multiplayer 2 2\n\n");
        return 1;
    }

    int start_id = atoi(argv[1]);
    int p2_count = atoi(argv[2]);

    if (start_id < 0 || start_id >= MAX_PLAYERS) {
        fprintf(stderr, "  [ERROR] Invalid start_player_id (0-%d)\n", MAX_PLAYERS - 1);
        return 1;
    }
    if (p2_count < 1 || start_id + p2_count > MAX_PLAYERS) {
        fprintf(stderr, "  [ERROR] Invalid player_count\n");
        return 1;
    }

    gs = shm_attach(false);
    if (!gs) {
        fprintf(stderr, "\n  \033[1;31m[ERROR] Failed to attach shared memory.\033[0m\n");
        fprintf(stderr, "  Make sure the main game (./chrono_rift) is running first!\n\n");
        return 1;
    }

    signal(SIGTERM, handle_sigterm);
    signal(SIGPIPE, SIG_IGN);

    printf("\n");
    printf("  \033[1;33m╔══════════════════════════════════════════════════╗\033[0m\n");
    printf("  \033[1;33m║     CHRONO RIFT — Player 2 Connected!            ║\033[0m\n");
    printf("  \033[1;33m╚══════════════════════════════════════════════════╝\033[0m\n\n");
    printf("  Controlling hero(es): ");
    for (int i = 0; i < p2_count; ++i) {
        if (i > 0) printf(", ");
        printf("%s", gs->entities[start_id + i].name);
    }
    printf("\n");
    printf("  Waiting for your turn...\n\n");
    fflush(stdout);

    // Create one thread per hero controlled by P2
    pthread_t threads[MAX_PLAYERS];
    P2ThreadArg args[MAX_PLAYERS];

    for (int i = 0; i < p2_count; ++i) {
        args[i].entity_id    = start_id + i;
        args[i].player_index = start_id + i;
        pthread_create(&threads[i], nullptr, p2_thread_func, &args[i]);
    }

    // Wait for game to end
    while (!gs->game_over) {
        sleep(1);
    }

    printf("\n  \033[1;33m=== GAME ENDED ===\033[0m\n");
    if (gs->players_won)
        printf("  \033[1;32mResult: VICTORY!\033[0m\n");
    else if (gs->quit_requested)
        printf("  \033[1;33mResult: QUIT\033[0m\n");
    else
        printf("  \033[1;31mResult: DEFEAT\033[0m\n");
    printf("\n");

    for (int i = 0; i < p2_count; ++i) {
        pthread_cancel(threads[i]);
        pthread_join(threads[i], nullptr);
    }

    shm_detach(gs);
    return 0;
}
