#pragma once
#include <signal.h>

// Custom signals used by the game
// SIGRTMIN-based to avoid clashing with standard signals

// Sent from Arbiter → target process/thread to apply stun
#define SIG_STUN    (SIGRTMIN + 0)

// Sent from Arbiter → ASP to suspend (Ultimate Ability)
#define SIG_SUSPEND SIGSTOP

// Sent from Arbiter → ASP to resume after ultimate pause
#define SIG_RESUME  SIGCONT

// Sent from HIP → Arbiter to indicate quit
// Uses SIGTERM as required by spec

// Sentinel: Arbiter uses SIGALRM for the ultimate ability timer window
// (managed internally by Arbiter process)

// Sent from Arbiter → target to deliver stun end (wake up)
#define SIG_STUN_END (SIGRTMIN + 1)

// Used internally to notify NPC thread of turn assignment
#define SIG_TURN_NOTIFY (SIGRTMIN + 2)
