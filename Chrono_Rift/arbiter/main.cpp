/*
 * main.cpp
 * Chrono Rift — Entry point / launcher
 * Prompts for configuration and launches the Arbiter.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

// Clean up any stale shared memory on startup
static void cleanup_shm() {
    // Remove stale segment if exists
    system("ipcrm -M 0x43524946 2>/dev/null");
}

int main(int argc, char* argv[]) {
    printf("\n");
    printf("  ╔══════════════════════════════════════════════════════════╗\n");
    printf("  ║                                                          ║\n");
    printf("  ║          C H R O N O   R I F T                          ║\n");
    printf("  ║     CS 2006 Operating Systems — Spring 2026             ║\n");
    printf("  ║                                                          ║\n");
    printf("  ╚══════════════════════════════════════════════════════════╝\n\n");

    // Get roll number
    int roll_no = 0;
    if (argc >= 2) {
        roll_no = atoi(argv[1]);
    } else {
        printf("  Enter your Roll Number (seed): ");
        fflush(stdout);
        scanf("%d", &roll_no);
    }

    if (roll_no <= 0) {
        printf("  [ERROR] Invalid roll number.\n");
        return 1;
    }

    // Get player count
    int player_count = 0;
    printf("  Select party size (1-4 heroes): ");
    fflush(stdout);
    scanf("%d", &player_count);
    if (player_count < 1 || player_count > 4) {
        printf("  Invalid. Using 1 player.\n");
        player_count = 1;
    }

    // Randomize enemy count (seeded by roll number)
    srand((unsigned)roll_no);
    int enemy_count = (rand() % 8) + 2; // 2..9
    printf("  Enemy count this run: %d\n", enemy_count);

    // Multiplayer mode?
    int mp_mode = 0;
    printf("  Enable Local Multiplayer bonus mode? (0=No, 1=Yes): ");
    fflush(stdout);
    scanf("%d", &mp_mode);

    printf("\n  Starting Chrono Rift...\n");
    printf("  Roll No: %d | Players: %d | Enemies: %d | Multiplayer: %s\n\n",
           roll_no, player_count, enemy_count, mp_mode ? "Yes" : "No");

    // Clean up any stale IPC
    cleanup_shm();

    // Launch arbiter process
    char rn[16], pc[4], ec[4], mp[4];
    snprintf(rn, sizeof(rn), "%d", roll_no);
    snprintf(pc, sizeof(pc), "%d", player_count);
    snprintf(ec, sizeof(ec), "%d", enemy_count);
    snprintf(mp, sizeof(mp), "%d", mp_mode);

    pid_t arbiter_pid = fork();
    if (arbiter_pid == 0) {
        execl("./arbiter", "./arbiter", rn, pc, ec, mp, nullptr);
        perror("execl arbiter");
        exit(1);
    }

    if (arbiter_pid < 0) {
        perror("fork");
        return 1;
    }

    // Wait for arbiter (which manages everything)
    int status;
    waitpid(arbiter_pid, &status, 0);

    printf("\n  Chrono Rift has ended. Goodbye.\n");
    return 0;
}
