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

HandSign get_winner(HandSign sign1, HandSign sign2) {
    if (sign1 == sign2) return (HandSign)-1;

    if ((sign1 == ROCK && sign2 == SCISSORS) ||
        (sign1 == SCISSORS && sign2 == PAPER) ||
        (sign1 == PAPER && sign2 == ROCK)) {
        return sign1;
    }
    return sign2;
}

const char* gesture_name(HandSign sign) {
    switch(sign) {
        case ROCK: return "Камень";
        case SCISSORS: return "Ножницы";
        case PAPER: return "Бумага";
        default: return "Неизвестно";
    }
}

void fighter_cleanup() {
    if (arena) {
        munmap(arena, sizeof(Arena));
    }
    if (shm_fd != -1) {
        close(shm_fd);
    }
    if (arena_sem != SEM_FAILED) {
        sem_close(arena_sem);
    }
}

void signal_handler(int sig) {
    printf("Боец остановлен по сигналу %d.\n", sig);
    fighter_cleanup();
    exit(0);
}

int check_arena_exists() {
    int fd = shm_open(SHM_NAME, O_RDONLY, 0666);
    if (fd == -1) {
        return 0;
    }
    close(fd);
    return 1;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Использовано %s <ID_бойца>.\n", argv[0]);
        return 1;
    }

    int fighter_id = atoi(argv[1]);
    if (fighter_id < 0 || fighter_id >= MAX_FIGHTERS) {
        printf("Неверный ID бойца. Должен быть от 0 до %d\n", MAX_FIGHTERS-1);
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    srand(time(NULL) + fighter_id + getpid());

    int wait_attempts = 30;
    while (wait_attempts > 0) {
        if (check_arena_exists()) {
            break;
        }
        sleep(1);
        wait_attempts--;
    }

    if (!check_arena_exists()) {
        printf("Для бойца %d арена не создана.\n", fighter_id);
        return 1;
    }

    shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (shm_fd == -1) {
        printf("У бойца %d проблема с подключением к арене.\n", fighter_id);
        return 1;
    }

    arena = mmap(NULL, sizeof(Arena), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (arena == MAP_FAILED) {
        printf("У бойца %d проблема с отображением памяти.\n", fighter_id);
        close(shm_fd);
        return 1;
    }

    arena_sem = sem_open(SEM_NAME, 0);
    if (arena_sem == SEM_FAILED) {
        printf("У бойца %d проблема с подключением к семафору битвы.\n", fighter_id);
        fighter_cleanup();
        return 1;
    }

    int id_check_attempts = 30;
    while (id_check_attempts > 0) {
        sem_lock(arena_sem);
        if (arena->total_count > 0 && fighter_id < arena->total_count) {
            sem_unlock(arena_sem);
            break;
        }
        sem_unlock(arena_sem);
        sleep(1);
        id_check_attempts--;
    }

    sem_lock(arena_sem);
    if (fighter_id >= arena->total_count || arena->total_count == 0) {
        printf("У бойца %d недопустимый ID или турнир не готов.\n", fighter_id);
        sem_unlock(arena_sem);
        fighter_cleanup();
        return 1;
    }

    arena->fighters[fighter_id].connected = 1;
    sem_unlock(arena_sem);

    printf("Боец %d начал участие в турнире.\n", fighter_id);
    send_to_observer("Боец присоединился к турниру.", fighter_id, -1);

    while (1) {
        sem_lock(arena_sem);

        if (arena->finished) {
            sem_unlock(arena_sem);
            break;
        }

        if (!arena->fighters[fighter_id].active) {
            sem_unlock(arena_sem);
            printf("Боец %d выбыл из турнира.\n", fighter_id);
            send_to_observer("Боец выбыл из турнира.", fighter_id, -1);
            break;
        }

        if (arena->fighters[fighter_id].has_rival) {
            int rival_id = arena->fighters[fighter_id].rival_id;

            if (rival_id < 0 || rival_id >= arena->total_count ||
                !arena->fighters[rival_id].active) {
                arena->fighters[fighter_id].has_rival = 0;
                arena->fighters[fighter_id].rival_id = -1;
                sem_unlock(arena_sem);
                continue;
            }

            HandSign my_move = rand() % 3;
            HandSign rival_move = rand() % 3;
            HandSign winner_move = get_winner(my_move, rival_move);

            printf("Бой %d vs %d: %s vs %s -> ", fighter_id, rival_id,
                   gesture_name(my_move), gesture_name(rival_move));

            char battle_msg[256];
            snprintf(battle_msg, sizeof(battle_msg), "Бой: %s vs %s",
                    gesture_name(my_move), gesture_name(rival_move));
            send_to_observer(battle_msg, fighter_id, rival_id);

            if (winner_move == my_move) {
                printf("Победил Боец %d\n", fighter_id);
                char win_msg[256];
                snprintf(win_msg, sizeof(win_msg), "Победил Боец %d", fighter_id);
                send_to_observer(win_msg, fighter_id, rival_id);
                arena->fighters[fighter_id].victories++;
                arena->fighters[rival_id].active = 0;
                arena->alive_count--;
            } else if (winner_move == rival_move) {
                printf("Победил Боец %d\n", rival_id);
                char win_msg[256];
                snprintf(win_msg, sizeof(win_msg), "Победил Боец %d", rival_id);
                send_to_observer(win_msg, fighter_id, rival_id);
                arena->fighters[rival_id].victories++;
                arena->fighters[fighter_id].active = 0;
                arena->alive_count--;
            } else {
                printf("Ничья\n");
                send_to_observer("Ничья", fighter_id, rival_id);
            }

            arena->fighters[fighter_id].has_rival = 0;
            arena->fighters[fighter_id].rival_id = -1;
            arena->fighters[rival_id].has_rival = 0;
            arena->fighters[rival_id].rival_id = -1;
        }

        sem_unlock(arena_sem);

        struct timespec delay = {0, 100000000};
        int sleep_result;
        do {
            sleep_result = nanosleep(&delay, &delay);
        } while (sleep_result == -1 && errno == EINTR);

        if (!check_arena_exists()) {
            printf("На бойце %d арена уничтожена.\n", fighter_id);
            break;
        }
    }

    printf("Боец %d завершил участие.\n", fighter_id);
    fighter_cleanup();
    return 0;
}