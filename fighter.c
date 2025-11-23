#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#define MAX_FIGHTERS 32
#define MSG_SIZE 256
#define OBSERVER_PATH_BASE "/tmp/battle_observer"
#define MAX_OBSERVERS 10

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
} Combatant;

typedef struct {
    Combatant fighters[MAX_FIGHTERS];
    int total_count;
    int alive_count;
    int round_num;
    int finished;
    int terminated;
} Arena;

typedef struct {
    char text[MSG_SIZE];
    int from_id;
    int against_id;
    int round_count;
    int is_result;
    HandSign move1;
    HandSign move2;
    int duel_rounds;
} DuelMessage;

Arena *combat_zone;
int zone_fd;
sem_t *combat_sem;

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

void send_to_watchers(const char* message, int from_id, int against_id,
                      int is_result, HandSign move1, HandSign move2, int duel_rounds) {
    DuelMessage msg;
    strncpy(msg.text, message, MSG_SIZE-1);
    msg.text[MSG_SIZE-1] = '\0';
    msg.from_id = from_id;
    msg.against_id = against_id;
    msg.round_count = combat_zone->round_num;
    msg.is_result = is_result;
    msg.move1 = move1;
    msg.move2 = move2;
    msg.duel_rounds = duel_rounds;

    for (int i = 0; i < MAX_OBSERVERS; i++) {
        char pipe_path[64];
        snprintf(pipe_path, sizeof(pipe_path), "%s_%d", OBSERVER_PATH_BASE, i);
        int pipe_fd = open(pipe_path, O_WRONLY | O_NONBLOCK);
        if (pipe_fd != -1) {
            ssize_t written = write(pipe_fd, &msg, sizeof(DuelMessage));
            if (written != sizeof(DuelMessage)) {
            }
            close(pipe_fd);
        }
    }
}

void fighter_cleanup() {
    if (combat_zone) {
        munmap(combat_zone, sizeof(Arena));
    }
    if (zone_fd != -1) {
        close(zone_fd);
    }
    if (combat_sem != SEM_FAILED) {
        sem_close(combat_sem);
    }
}

void signal_handler(int sig) {
    printf("Боец: останов по сигналу %d\n", sig);
    fighter_cleanup();
    exit(0);
}

HandSign get_winner(HandSign sign1, HandSign sign2) {
    if (sign1 == sign2) return -1;

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

int check_zone_exists() {
    int fd = shm_open("/battle_ground", O_RDONLY, 0666);
    if (fd == -1) {
        return 0;
    }
    close(fd);
    return 1;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Использование: %s <ID_бойца>\n", argv[0]);
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

    if (!check_zone_exists()) {
        printf("Боец %d: арена не создана\n", fighter_id);
        return 1;
    }

    zone_fd = shm_open("/battle_ground", O_RDWR, 0666);
    if (zone_fd == -1) {
        printf("Боец %d: проблема с подключением к арене\n", fighter_id);
        return 1;
    }

    combat_zone = mmap(NULL, sizeof(Arena), PROT_READ | PROT_WRITE, MAP_SHARED, zone_fd, 0);
    if (combat_zone == MAP_FAILED) {
        printf("Боец %d: проблема с отображением памяти\n", fighter_id);
        close(zone_fd);
        return 1;
    }

    combat_sem = sem_open("/battle_semaphore", 0);
    if (combat_sem == SEM_FAILED) {
        printf("Боец %d: проблема с подключением к семафору битвы\n", fighter_id);
        fighter_cleanup();
        return 1;
    }

    sem_lock(combat_sem);
    if (fighter_id >= combat_zone->total_count) {
        printf("Боец %d: недопустимый ID для этого турнира (макс: %d)\n", fighter_id, combat_zone->total_count-1);
        sem_unlock(combat_sem);
        fighter_cleanup();
        return 1;
    }
    sem_unlock(combat_sem);

    printf("Боец %d начал участие в турнире\n", fighter_id);
    send_to_watchers("Боец присоединился к турниру", fighter_id, -1, 0, ROCK, ROCK, 0);

    while (1) {
        sem_lock(combat_sem);

        if (combat_zone->finished || combat_zone->terminated) {
            sem_unlock(combat_sem);
            break;
        }

        if (!combat_zone->fighters[fighter_id].active) {
            sem_unlock(combat_sem);
            send_to_watchers("Боец выбыл из турнира", fighter_id, -1, 0, ROCK, ROCK, 0);
            break;
        }

        if (combat_zone->fighters[fighter_id].has_rival) {
            int rival_id = combat_zone->fighters[fighter_id].rival_id;

            if (rival_id < 0 || rival_id >= combat_zone->total_count ||
                !combat_zone->fighters[rival_id].active) {
                combat_zone->fighters[fighter_id].has_rival = 0;
                combat_zone->fighters[fighter_id].rival_id = -1;
                sem_unlock(combat_sem);
                continue;
            }

            HandSign my_move, rival_move, winner_move;
            int round_count = 0;

            do {
                round_count++;
                my_move = rand() % 3;
                rival_move = rand() % 3;
                winner_move = get_winner(my_move, rival_move);

                combat_zone->fighters[fighter_id].gesture = my_move;
                combat_zone->fighters[rival_id].gesture = rival_move;

                char message[MSG_SIZE];
                if (round_count == 1) {
                    snprintf(message, MSG_SIZE, "Начало боя между Бойцом %d и Бойцом %d", fighter_id, rival_id);
                    send_to_watchers(message, fighter_id, rival_id, 0, my_move, rival_move, round_count);
                }

                if (winner_move == -1) {
                    snprintf(message, MSG_SIZE, "Ничья в раунде %d боя %d vs %d", round_count, fighter_id, rival_id);
                    send_to_watchers(message, fighter_id, rival_id, 0, my_move, rival_move, round_count);

                    struct timespec delay = {0, 500000000};
                    int sleep_result;
                    do {
                        sleep_result = nanosleep(&delay, &delay);
                    } while (sleep_result == -1 && errno == EINTR);

                    if (combat_zone->finished || combat_zone->terminated ||
                        !combat_zone->fighters[fighter_id].active ||
                        !combat_zone->fighters[rival_id].active) {
                        break;
                    }
                }
            } while (winner_move == -1);

            if (combat_zone->finished || combat_zone->terminated ||
                !combat_zone->fighters[fighter_id].active) {
                sem_unlock(combat_sem);
                break;
            }

            if (winner_move != -1) {
                char message[MSG_SIZE];
                if (winner_move == my_move) {
                    snprintf(message, MSG_SIZE, "Боец %d победил Бойца %d за %d раундов", fighter_id, rival_id, round_count);
                    combat_zone->fighters[fighter_id].victories++;
                    combat_zone->fighters[rival_id].active = 0;
                    combat_zone->alive_count--;
                } else {
                    snprintf(message, MSG_SIZE, "Боец %d победил Бойца %d за %d раундов", rival_id, fighter_id, round_count);
                    combat_zone->fighters[rival_id].victories++;
                    combat_zone->fighters[fighter_id].active = 0;
                    combat_zone->alive_count--;
                }

                send_to_watchers(message, fighter_id, rival_id, 1, my_move, rival_move, round_count);
            }

            combat_zone->fighters[fighter_id].has_rival = 0;
            combat_zone->fighters[fighter_id].rival_id = -1;
            combat_zone->fighters[rival_id].has_rival = 0;
            combat_zone->fighters[rival_id].rival_id = -1;
        }

        sem_unlock(combat_sem);

        struct timespec delay = {0, 300000000};
        int sleep_result;
        do {
            sleep_result = nanosleep(&delay, &delay);
        } while (sleep_result == -1 && errno == EINTR);

        if (!check_zone_exists()) {
            printf("Боец %d: арена уничтожена\n", fighter_id);
            break;
        }
    }

    printf("Боец %d завершил участие\n", fighter_id);
    fighter_cleanup();
    return 0;
}