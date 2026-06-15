#pragma once
#include <cstring>
#include <semaphore.h>
#include <pthread.h>
#include <sys/types.h>
#include <atomic>

// ─── Constants ────────────────────────────────────────────────────────────────
static constexpr int MAX_PLAYERS       = 4;
static constexpr int MAX_ENEMIES       = 9;
static constexpr int MAX_ENTITIES      = MAX_PLAYERS + MAX_ENEMIES;
static constexpr int INVENTORY_SLOTS   = 20;
static constexpr int MAX_LTS_WEAPONS   = 64;   // long-term storage per player
static constexpr int ACTION_LOG_LINES  = 30;
static constexpr int ACTION_LOG_LEN    = 128;
static constexpr int MAX_NAME_LEN      = 32;
static constexpr int WIN_KILL_COUNT    = 10;
static constexpr int MAX_PENDING_LOOT  = 10;

// Stamina constants
static constexpr int PLAYER_MAX_STAMINA = 100;
static constexpr int ENEMY_MAX_STAMINA  = 150;

// Stun duration (seconds)
static constexpr int STUN_DURATION_SEC  = 3;
// Ultimate pause duration (seconds)
static constexpr int ULTIMATE_PAUSE_SEC = 10;
// NPC turn timeout (seconds)
static constexpr int NPC_TIMEOUT_SEC    = 3;

// ─── Weapon IDs ───────────────────────────────────────────────────────────────
enum class WeaponID : int {
    NONE         = 0,
    SOLAR_CORE   = 1,
    LUNAR_BLADE  = 2,
    IRON_HALBERD = 3,
    VENOM_DAGGER = 4,
    THUNDERSTAFF = 5,
    OBSIDIAN_AXE = 6,
    FROSTBOW     = 7,
    SPLINTER_STICK = 8,
    ECLIPSE_RELIC  = 9,
};

struct WeaponInfo {
    WeaponID id;
    char     name[MAX_NAME_LEN];
    int      slot_size;
    int      damage;
};

inline constexpr WeaponInfo WEAPON_TABLE[] = {
    { WeaponID::NONE,          "None",          0,  0  },
    { WeaponID::SOLAR_CORE,    "Solar Core",   10, 95  },
    { WeaponID::LUNAR_BLADE,   "Lunar Blade",  10, 90  },
    { WeaponID::IRON_HALBERD,  "Iron Halberd",  7, 55  },
    { WeaponID::VENOM_DAGGER,  "Venom Dagger",  4, 30  },
    { WeaponID::THUNDERSTAFF,  "Thunderstaff",  6, 50  },
    { WeaponID::OBSIDIAN_AXE,  "Obsidian Axe",  5, 45  },
    { WeaponID::FROSTBOW,      "Frostbow",       6, 48  },
    { WeaponID::SPLINTER_STICK,"Splinter Stick", 2, 12  },
    { WeaponID::ECLIPSE_RELIC, "Eclipse Relic",  5, 60  },
};

inline const WeaponInfo& getWeaponInfo(WeaponID id) {
    for (auto& w : WEAPON_TABLE) {
        if (w.id == id) return w;
    }
    return WEAPON_TABLE[0];
}

// ─── Inventory ────────────────────────────────────────────────────────────────
// Slot stores the weapon occupying it (NONE = free)
struct Inventory {
    WeaponID slots[INVENTORY_SLOTS];   // primary 20-slot array
    WeaponID lts[MAX_LTS_WEAPONS];     // long-term storage
    int      lts_count;

    void clear() {
        for (auto& s : slots) s = WeaponID::NONE;
        for (auto& s : lts)   s = WeaponID::NONE;
        lts_count = 0;
    }

    // Returns starting slot index of placed weapon, or -1 on failure.
    // Swaps out weapons to LTS if needed (swaps minimum required).
    int placeWeapon(WeaponID wid);
    bool removeWeapon(WeaponID wid);          // remove first occurrence
    bool hasWeapon(WeaponID wid) const;
    bool swapIn(WeaponID wid);                // bring from LTS to primary

private:
    int findContiguousFree(int need) const;
    // returns list of weapons that can be evicted to free `need` slots
    // minimal eviction: evict smallest weapons first
    bool evictForSpace(int need);
};

// ─── Entity ───────────────────────────────────────────────────────────────────
enum class EntityType : int { PLAYER = 0, ENEMY = 1 };
enum class EntityState : int { ALIVE = 0, DEAD = 1, STUNNED = 2 };

struct Entity {
    int         id;           // unique 0-based index
    EntityType  type;
    EntityState state;
    char        name[MAX_NAME_LEN];

    int  hp;
    int  max_hp;
    int  damage;
    int  speed;
    int  max_stamina;
    int  stamina;            // current stamina (0 .. max_stamina)
    bool stun_active;
    // stun end time (absolute, seconds since epoch) — set by Arbiter
    time_t stun_end_time;

    pid_t process_pid;       // PID of controlling process
    pthread_t thread_id;     // thread id within that process (enemy/player)
    int  player_index;       // for players only (0-based)

    Inventory inventory;     // per-entity inventory

    bool is_alive() const { return state != EntityState::DEAD; }
};

// ─── Action ───────────────────────────────────────────────────────────────────
enum class ActionType : int {
    STRIKE           = 0,
    EXHAUST          = 1,
    USE_WEAPON       = 2,
    SWAP_IN          = 3,
    HEAL             = 4,
    SKIP             = 5,
    ULTIMATE         = 6,
    QUIT             = 7,
    ACQUIRE_ARTIFACT = 8,
};

struct Action {
    int        actor_id;     // entity id performing the action
    ActionType type;
    int        target_id;    // for strike/exhaust/use_weapon
    WeaponID   weapon_id;    // for use_weapon / swap_in
    bool       valid;        // set true by HIP when action is ready
};

// ─── Artifact Resource Table ──────────────────────────────────────────────────
struct ArtifactEntry {
    WeaponID weapon_id;
    int      held_by;        // entity id, or -1 if free
    bool     exists;         // Eclipse Relic may not exist yet
};

struct ResourceTable {
    ArtifactEntry artifacts[3]; // Solar Core, Lunar Blade, Eclipse Relic
    pthread_mutex_t lock;       // protects the whole table
};

// Pending loot from enemy kills — player chooses to pick up or decline
struct PendingLoot {
    bool     active;
    int      for_player;                // entity id of killer
    WeaponID weapons[MAX_PENDING_LOOT];
    int      count;
};

// ─── Global Game State (lives in shared memory) ───────────────────────────────
struct GameState {
    // Process PIDs
    pid_t arbiter_pid;
    pid_t hip_pid;       // Human Interfacing Process
    pid_t asp_pid;       // Automated Strategic Process

    // Entities
    Entity  entities[MAX_ENTITIES];
    int     player_count;
    int     enemy_count;
    int     total_enemies_killed;

    // Turn management
    int     current_actor_id;   // who is scheduled to act next (-1 = none yet)
    bool    action_pending;     // HIP/ASP set true; Arbiter clears after processing
    Action  pending_action;

    // Artifact table
    ResourceTable resource_table;

    // Eclipse Relic dropped flag
    bool eclipse_relic_in_world;
    int  eclipse_relic_tile;    // unused in terminal mode, for rendering

    // Game lifecycle
    bool game_over;
    bool players_won;
    bool quit_requested;

    // Multiplayer mode flag
    bool multiplayer_mode;

    // Action log (ring buffer)
    char   log_lines[ACTION_LOG_LINES][ACTION_LOG_LEN];
    int    log_head;   // index of oldest entry
    int    log_count;  // number of valid entries

    // Synchronization (memory-based, no pipes)
    pthread_mutex_t state_lock;
    pthread_mutex_t log_lock;
    pthread_mutex_t action_lock;
    pthread_cond_t  action_ready_cond;   // Arbiter waits for action
    pthread_cond_t  turn_assigned_cond;  // HIP/ASP wait for their turn

    // Semaphore for rendering thread
    sem_t render_sem;

    // HIP input buffer per player thread (legacy terminal mode)
    char   input_buf[MAX_PLAYERS][256];
    bool   input_ready[MAX_PLAYERS];
    pthread_mutex_t input_lock[MAX_PLAYERS];
    pthread_cond_t  input_cond[MAX_PLAYERS];

    // GUI input: renderer writes here, HIP/multiplayer reads
    bool   gui_mode;  // true = read input from GUI, false = stdin
    struct GUIInput {
        bool       ready;    // renderer sets true after click
        Action     action;   // the chosen action
    } gui_input[MAX_PLAYERS];
    pthread_mutex_t gui_lock[MAX_PLAYERS];
    pthread_cond_t  gui_cond[MAX_PLAYERS];

    // Which player the GUI is currently showing the menu for
    int    gui_viewing_player;  // entity id of player whose inventory/tab is shown

    // Ultimate pause state
    bool   ultimate_pause_active;

    // Pending loot awaiting player pickup decision
    PendingLoot pending_loot;

    // Deadlock detection helper: who waits for which artifact
    int    waiting_for_artifact[MAX_ENTITIES]; // -1 = not waiting
};

// ─── Shared memory key ───────────────────────────────────────────────────────
static constexpr key_t SHM_KEY = 0x43524946; // "CRIF"
static constexpr int   SHM_SIZE = sizeof(GameState);
