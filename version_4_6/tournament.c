#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <semaphore.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>

#define MAX_FIGHTERS 32
#define SHM_NAME "/tournament_shm_46"
#define SEM_NAME "/tournament_sem_46"

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
} Arena;

Arena *combat_zone;
int shm_fd;
sem_t *combat_sem;
pid_t fighter_pids[MAX_FIGHTERS];
int fighter_count;

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

void fighter_process(int fighter_id) {
    printf("Боец %d (PID %d) начал участие в турнире.\n", fighter_id, getpid());

    while (1) {
        sem_lock(combat_sem);

        if (combat_zone->finished) {
            sem_unlock(combat_sem);
            break;
        }

        if (!combat_zone->fighters[fighter_id].active) {
            sem_unlock(combat_sem);
            break;
        }

        if (combat_zone->fighters[fighter_id].has_rival) {
            int rival_id = combat_zone->fighters[fighter_id].rival_id;

            if (rival_id >= 0 && rival_id < combat_zone->total_count &&
                combat_zone->fighters[rival_id].has_rival && 
                combat_zone->fighters[rival_id].rival_id == fighter_id &&
                fighter_id > rival_id) {
                combat_zone->fighters[fighter_id].has_rival = 0;
                combat_zone->fighters[fighter_id].rival_id = -1;
                sem_unlock(combat_sem);
                continue;
            }

            if (rival_id >= 0 && rival_id < combat_zone->total_count &&
                combat_zone->fighters[rival_id].active) {

                HandSign my_move = rand() % 3;
                HandSign rival_move = rand() % 3;
                HandSign winner = get_winner(my_move, rival_move);
                int duel_rounds = 0;

                do {
                    duel_rounds++;
                    my_move = rand() % 3;
                    rival_move = rand() % 3;
                    winner = get_winner(my_move, rival_move);

                    printf("Бой %d vs %d (раунд %d): %s vs %s => ",
                        fighter_id, rival_id, duel_rounds,
                        gesture_name(my_move), gesture_name(rival_move));

                    if (winner == my_move) {
                        printf("Победил Боец %d\n", fighter_id);
                        combat_zone->fighters[fighter_id].victories++;
                        combat_zone->fighters[rival_id].active = 0;
                        combat_zone->alive_count--;
                    } else if (winner == rival_move) {
                        printf("Победил Боец %d\n", rival_id);
                        combat_zone->fighters[rival_id].victories++;
                        combat_zone->fighters[fighter_id].active = 0;
                        combat_zone->alive_count--;
                    } else {
                        printf("Ничья\n");
                        usleep(300000);
                    }
                } while (winner == (HandSign)-1);

                combat_zone->fighters[fighter_id].has_rival = 0;
                combat_zone->fighters[fighter_id].rival_id = -1;
                combat_zone->fighters[rival_id].has_rival = 0;
                combat_zone->fighters[rival_id].rival_id = -1;
            }
        }

        sem_unlock(combat_sem);
        usleep(100000);
    }

    printf("Боец %d завершил участие.\n", fighter_id);
}

void setup_round() {
    sem_lock(combat_sem);

    if (combat_zone->finished) {
        sem_unlock(combat_sem);
        return;
    }

    int ready_fighters[MAX_FIGHTERS];
    int count = 0;

    for (int i = 0; i < combat_zone->total_count; i++) {
        if (combat_zone->fighters[i].active && !combat_zone->fighters[i].has_rival) {
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

        printf("Организован бой: Боец %d vs Боец %d\n", fighter1, fighter2);
    }

    combat_zone->round_num++;
    printf("Начало раунда %d. Бойцов готово к бою: %d\n", combat_zone->round_num, count);

    sem_unlock(combat_sem);
}

void kill_fighters() {
    for (int i = 0; i < fighter_count; i++) {
        if (fighter_pids[i] > 0) {
            kill(fighter_pids[i], SIGTERM);
        }
    }
}

void cleanup() {
    printf("Очистка ресурсов.\n");
    kill_fighters();

    for (int i = 0; i < fighter_count; i++) {
        wait(NULL);
    }

    if (combat_zone) {
        munmap(combat_zone, sizeof(Arena));
    }
    if (shm_fd != -1) {
        close(shm_fd);
        shm_unlink(SHM_NAME);
    }
}

void signal_handler(int sig) {
    printf("Турнир остановлен по сигналу %d.\n", sig);
    sem_lock(combat_sem);
    combat_zone->finished = 1;
    sem_unlock(combat_sem);
    sleep(1);
    cleanup();
    exit(0);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Использовано %s <количество_бойцов>.\n", argv[0]);
        return 1;
    }

    fighter_count = atoi(argv[1]);
    if (fighter_count < 2 || fighter_count > MAX_FIGHTERS) {
        printf("Количество бойцов должно быть от 2 до %d.\n", MAX_FIGHTERS);
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    srand(time(NULL));

    printf("------ Центр управления турниром ------\n");
    printf("Количество участников: %d.\n", fighter_count);

    shm_unlink(SHM_NAME);


    shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("Проблема с созданием разделяемой памяти.");
        return 1;
    }

    if (ftruncate(shm_fd, sizeof(Arena)) == -1) {
        perror("Проблема с установкой размера памяти.");
        close(shm_fd);
        return 1;
    }

    combat_zone = mmap(NULL, sizeof(Arena), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (combat_zone == MAP_FAILED) {
        perror("Проблема с отображением памяти.");
        close(shm_fd);
        return 1;
    }

    memset(combat_zone, 0, sizeof(Arena));
    combat_zone->total_count = fighter_count;
    combat_zone->alive_count = fighter_count;

    for (int i = 0; i < fighter_count; i++) {
        combat_zone->fighters[i].id = i;
        combat_zone->fighters[i].active = 1;
        combat_zone->fighters[i].victories = 0;
        combat_zone->fighters[i].gesture = ROCK;
        combat_zone->fighters[i].has_rival = 0;
        combat_zone->fighters[i].rival_id = -1;
    }

    sem_t sem;
    combat_sem = &sem;
    if (sem_init(combat_sem, 1, 1) == -1) {
        perror("Проблема с созданием семафора.");
        cleanup();
        return 1;
    }

    for (int i = 0; i < fighter_count; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            fighter_process(i);
            exit(0);
        } else if (pid < 0) {
            perror("Проблема с созданием процесса бойца.");
            return 1;
        } else {
            fighter_pids[i] = pid;
        }
    }

    printf("Все бойцы созданы. Начало турнира...\n");
    sleep(2);

    int round = 0;
    while (!combat_zone->finished) {
        sem_lock(combat_sem);
        int active = combat_zone->alive_count;
        sem_unlock(combat_sem);

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
        int max_waits = 30;
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
                sleep(1);
                max_waits--;
            }
        } while (duels_active && max_waits > 0);

        sem_lock(combat_sem);
        printf("Промежуточные победители: ");
        int first = 1;
        for (int i = 0; i < fighter_count; i++) {
            if (combat_zone->fighters[i].active) {
                if (!first) {
                    printf(", ");
                }
                printf("Боец %d", i);
                first = 0;
            }
        }
        printf("\n");
        sem_unlock(combat_sem);
    }

    sem_lock(combat_sem);
    int winner_found = 0;
    for (int i = 0; i < fighter_count; i++) {
        if (combat_zone->fighters[i].active) {
            printf("\nТурнир завершен! Победитель: Боец %d\n", i);
            winner_found = 1;
            break;
        }
    }

    if (!winner_found) {
        printf("\nТурнир завершен! Победитель не определен.\n");
    }
    sem_unlock(combat_sem);

    printf("Все бои завершены.\n");
    sleep(2);

    sem_lock(combat_sem);
    combat_zone->finished = 1;
    sem_unlock(combat_sem);

    sleep(1);
    cleanup();

    for (int i = 0; i < fighter_count; i++) {
        wait(NULL);
    }

    return 0;
}
