#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <errno.h>

#define MAX_FIGHTERS 32

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

sem_t *combat_sem;
sem_t *display_sem;
Arena *combat_zone;
int zone_fd;

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

void zone_cleanup() {
    printf("Очистка ресурсов турнира\n");

    if (combat_sem != NULL) {
        sem_destroy(combat_sem);
        free(combat_sem);
    }
    if (display_sem != NULL) {
        sem_destroy(display_sem);
        free(display_sem);
    }
    if (combat_zone) {
        munmap(combat_zone, sizeof(Arena));
    }
    if (zone_fd != -1) {
        close(zone_fd);
        shm_unlink("/battle_zone");
    }
}

void signal_handler(int sig) {
    printf("Турнир остановлен по сигналу %d\n", sig);

    sem_lock(combat_sem);
    if (combat_zone) {
        combat_zone->finished = 1;
        combat_zone->terminated = 1;
    }
    sem_unlock(combat_sem);

    zone_cleanup();
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

void fighter_process(int id) {
    srand(time(NULL) + id + getpid());

    printf("Боец %d начал участие в турнире\n", id);

    while (1) {
        sem_lock(combat_sem);

        if (combat_zone->finished || combat_zone->terminated) {
            sem_unlock(combat_sem);
            break;
        }

        if (!combat_zone->fighters[id].active) {
            sem_unlock(combat_sem);
            break;
        }

        if (combat_zone->fighters[id].has_rival) {
            int rival_id = combat_zone->fighters[id].rival_id;

            if (rival_id < 0 || rival_id >= combat_zone->total_count ||
                !combat_zone->fighters[rival_id].active) {
                combat_zone->fighters[id].has_rival = 0;
                combat_zone->fighters[id].rival_id = -1;
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

                combat_zone->fighters[id].gesture = my_move;
                combat_zone->fighters[rival_id].gesture = rival_move;

                sem_lock(display_sem);
                printf("Боец %d: %s против Бойца %d: %s",
                       id, gesture_name(my_move), rival_id, gesture_name(rival_move));

                if (winner_move == -1) {
                    printf(" -> Ничья! Раунд %d\n", round_count);
                } else if (winner_move == my_move) {
                    printf(" -> Победил Боец %d! (раунд %d)\n", id, round_count);
                } else {
                    printf(" -> Победил Боец %d! (раунд %d)\n", rival_id, round_count);
                }
                sem_unlock(display_sem);

                if (winner_move == -1) {
                    struct timespec delay = {0, 500000000};
                    int sleep_result;
                    do {
                        sleep_result = nanosleep(&delay, &delay);
                    } while (sleep_result == -1 && errno == EINTR);

                    if (combat_zone->finished || combat_zone->terminated ||
                        !combat_zone->fighters[id].active ||
                        !combat_zone->fighters[rival_id].active) {
                        break;
                    }
                }
            } while (winner_move == -1);

            if (combat_zone->finished || combat_zone->terminated ||
                !combat_zone->fighters[id].active) {
                sem_unlock(combat_sem);
                break;
            }

            if (winner_move != -1) {
                if (winner_move == my_move) {
                    combat_zone->fighters[id].victories++;
                    combat_zone->fighters[rival_id].active = 0;
                    combat_zone->alive_count--;
                    printf("Боец %d выбывает из турнира\n", rival_id);
                } else {
                    combat_zone->fighters[rival_id].victories++;
                    combat_zone->fighters[id].active = 0;
                    combat_zone->alive_count--;
                    printf("Боец %d выбывает из турнира\n", id);
                }
            }

            combat_zone->fighters[id].has_rival = 0;
            combat_zone->fighters[id].rival_id = -1;
            combat_zone->fighters[rival_id].has_rival = 0;
            combat_zone->fighters[rival_id].rival_id = -1;
        }

        sem_unlock(combat_sem);

        struct timespec delay = {0, 300000000};
        int sleep_result;
        do {
            sleep_result = nanosleep(&delay, &delay);
        } while (sleep_result == -1 && errno == EINTR);
    }

    printf("Боец %d завершил участие\n", id);
}

void setup_round() {
    sem_lock(combat_sem);

    if (combat_zone->finished || combat_zone->terminated) {
        sem_unlock(combat_sem);
        return;
    }

    int ready_fighters[MAX_FIGHTERS];
    int count = 0;

    for (int i = 0; i < combat_zone->total_count; i++) {
        if (combat_zone->fighters[i].active &&
            !combat_zone->fighters[i].has_rival) {
            ready_fighters[count++] = i;
        }
    }

    for (int i = count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int temp = ready_fighters[i];
        ready_fighters[i] = ready_fighters[j];
        ready_fighters[j] = temp;
    }

    for (int i = 0; i < count - 1; i += 2) {
        int fighter1 = ready_fighters[i];
        int fighter2 = ready_fighters[i + 1];

        combat_zone->fighters[fighter1].has_rival = 1;
        combat_zone->fighters[fighter1].rival_id = fighter2;
        combat_zone->fighters[fighter2].has_rival = 1;
        combat_zone->fighters[fighter2].rival_id = fighter1;

        sem_lock(display_sem);
        printf("Организован бой: Боец %d vs Боец %d\n", fighter1, fighter2);
        sem_unlock(display_sem);
    }

    if (count % 2 == 1 && count > 1) {
        int lucky_fighter = ready_fighters[count - 1];
        sem_lock(display_sem);
        printf("Боец %d проходит автоматически в следующий раунд\n", lucky_fighter);
        sem_unlock(display_sem);
    }

    combat_zone->round_num++;
    sem_unlock(combat_sem);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Использование: %s <количество_бойцов>\n", argv[0]);
        return 1;
    }

    int fighter_count = atoi(argv[1]);
    if (fighter_count < 2 || fighter_count > MAX_FIGHTERS) {
        printf("Количество бойцов должно быть от 2 до %d\n", MAX_FIGHTERS);
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    srand(time(NULL) + getpid());

    shm_unlink("/battle_zone");

    zone_fd = shm_open("/battle_zone", O_CREAT | O_RDWR, 0666);
    if (zone_fd == -1) {
        perror("Проблема с созданием разделяемой памяти");
        return 1;
    }

    if (ftruncate(zone_fd, sizeof(Arena)) == -1) {
        perror("Проблема с установкой размера разделяемой памяти");
        close(zone_fd);
        return 1;
    }

    combat_zone = mmap(NULL, sizeof(Arena), PROT_READ | PROT_WRITE, MAP_SHARED, zone_fd, 0);
    if (combat_zone == MAP_FAILED) {
        perror("Проблема с отображением памяти");
        close(zone_fd);
        return 1;
    }

    combat_sem = malloc(sizeof(sem_t));
    display_sem = malloc(sizeof(sem_t));

    if (sem_init(combat_sem, 1, 1) == -1) {
        perror("Проблема с созданием семафора битвы");
        zone_cleanup();
        return 1;
    }

    if (sem_init(display_sem, 1, 1) == -1) {
        perror("Проблема с созданием семафора отображения");
        zone_cleanup();
        return 1;
    }

    memset(combat_zone, 0, sizeof(Arena));
    combat_zone->total_count = fighter_count;
    combat_zone->alive_count = fighter_count;
    combat_zone->round_num = 0;
    combat_zone->finished = 0;
    combat_zone->terminated = 0;

    for (int i = 0; i < fighter_count; i++) {
        combat_zone->fighters[i].id = i;
        combat_zone->fighters[i].active = 1;
        combat_zone->fighters[i].victories = 0;
        combat_zone->fighters[i].gesture = ROCK;
        combat_zone->fighters[i].has_rival = 0;
        combat_zone->fighters[i].rival_id = -1;
    }

    printf("Турнир Камень-Ножницы-Бумага начинается\n");
    printf("Количество участников: %d\n", fighter_count);

    pid_t pids[MAX_FIGHTERS];
    for (int i = 0; i < fighter_count; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            fighter_process(i);
            exit(0);
        } else if (pid < 0) {
            perror("Проблема с созданием процесса");
            for (int j = 0; j < i; j++) {
                kill(pids[j], SIGTERM);
            }
            zone_cleanup();
            return 1;
        } else {
            pids[i] = pid;
        }
    }

    printf("Все бойцы готовы. Начинаем турнир\n\n");

    int round = 0;
    while (!combat_zone->finished) {
        sem_lock(combat_sem);
        int active = combat_zone->alive_count;
        int ended = combat_zone->finished;
        sem_unlock(combat_sem);

        if (ended) break;

        if (active <= 1) {
            sem_lock(combat_sem);
            combat_zone->finished = 1;
            sem_unlock(combat_sem);
            break;
        }

        printf("\n--- Раунд %d ---\n", ++round);
        printf("Активных бойцов: %d\n", active);

        setup_round();

        sleep(3);

        int duels_active;
        int max_waits = 60;
        do {
            duels_active = 0;
            sem_lock(combat_sem);
            for (int i = 0; i < fighter_count; i++) {
                if (combat_zone->fighters[i].has_rival) {
                    duels_active = 1;
                    break;
                }
            }
            sem_unlock(combat_sem);
            if (duels_active) {
                usleep(500000);
                max_waits--;
            }
        } while (duels_active && max_waits > 0);
    }

    sem_lock(combat_sem);
    for (int i = 0; i < fighter_count; i++) {
        if (combat_zone->fighters[i].active) {
            printf("\nТУРНИР ЗАВЕРШЕН! ПОБЕДИТЕЛЬ: БОЕЦ %d\n", i);
            printf("Количество побед: %d\n", combat_zone->fighters[i].victories);
            break;
        }
    }
    sem_unlock(combat_sem);

    for (int i = 0; i < fighter_count; i++) {
        int status;
        waitpid(pids[i], &status, 0);
    }

    printf("Турнир завершен. Очистка ресурсов\n");
    zone_cleanup();

    return 0;
}