#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>

#define MAX_FIGHTERS 32
#define SHM_NAME "/battle_arena_9"
#define SEM_NAME "/battle_sem_9"
#define PIPE_NAME "/tmp/tournament_observer_9"

typedef enum {
    ROCK = 0,
    SCISSORS = 1,
    PAPER = 2
} HandSign;

typedef struct {
    int id;
    int active;
    int victories;
    HandSign gesture;
    int has_rival;
    int rival_id;
    int connected;
} Combatant;

typedef struct {
    Combatant fighters[MAX_FIGHTERS];
    int total_count;
    int alive_count;
    int round_num;
    int finished;
} Arena;

Arena *arena;
int shm_fd;
sem_t *arena_sem;

void sem_lock(sem_t *sem) {
    int result;
    do {
        result = sem_wait(sem);
    } while (result == -1 && errno == EINTR);
}

void sem_unlock(sem_t *sem) {
    int result;
    do {
        result = sem_post(sem);
    } while (result == -1 && errno == EINTR);
}

void send_to_observer(const char* message, int from_id, int against_id) {
    int pipe_fd = open(PIPE_NAME, O_WRONLY | O_NONBLOCK);
    if (pipe_fd != -1) {
        char full_msg[256];
        if (from_id == -1 && against_id == -1) {
            snprintf(full_msg, sizeof(full_msg), "%s", message);
        } else if (against_id == -1) {
            snprintf(full_msg, sizeof(full_msg), "Боец %d: %s", from_id, message);
        } else {
            snprintf(full_msg, sizeof(full_msg), "Бой %d vs %d: %s", from_id, against_id, message);
        }
        write(pipe_fd, full_msg, strlen(full_msg) + 1);
        close(pipe_fd);
    }
}

void cleanup_resources() {
    printf("Очистка ресурсов.\n");
    if (arena) {
        munmap(arena, sizeof(Arena));
    }
    if (shm_fd != -1) {
        close(shm_fd);
        shm_unlink(SHM_NAME);
    }
    if (arena_sem != SEM_FAILED) {
        sem_close(arena_sem);
        sem_unlink(SEM_NAME);
    }
    unlink(PIPE_NAME);
}

void signal_handler(int sig) {
    printf("Турнир остановлен по сигналу %d.\n", sig);
    sem_lock(arena_sem);
    arena->finished = 1;
    sem_unlock(arena_sem);
    send_to_observer("Турнир остановлен по сигналу.", -1, -1);
    sleep(1);
    cleanup_resources();
    exit(0);
}

int get_connected_count() {
    int count = 0;
    sem_lock(arena_sem);
    for (int i = 0; i < arena->total_count; i++) {
        if (arena->fighters[i].connected) {
            count++;
        }
    }
    sem_unlock(arena_sem);
    return count;
}

void print_active_fighters() {
    sem_lock(arena_sem);
    printf("Промежуточные победители: ");
    int first = 1;
    for (int i = 0; i < arena->total_count; i++) {
        if (arena->fighters[i].active) {
            if (!first) {
                printf(", ");
            }
            printf("Боец %d", i);
            first = 0;
        }
    }
    printf("\n");
    sem_unlock(arena_sem);
}

void setup_round() {
    sem_lock(arena_sem);

    if (arena->finished) {
        sem_unlock(arena_sem);
        return;
    }

    int ready_fighters[MAX_FIGHTERS];
    int count = 0;

    for (int i = 0; i < arena->total_count; i++) {
        if (arena->fighters