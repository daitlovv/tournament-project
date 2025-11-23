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
#define SHM_NAME "/battle_arena_78"
#define SEM_NAME "/battle_sem_78"

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
}

void signal_handler(int sig) {
    printf("Турнир остановлен по сигналу %d.\n", sig);
    sem_lock(arena_sem);
    arena->finished = 1;
    sem_unlock(arena_sem);
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
        if (arena->fighters[i].active && !arena->fighters[i].has_rival) {
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

        arena->fighters[fighter1].has_rival = 1;
        arena->fighters[fighter1].rival_id = fighter2;
        arena->fighters[fighter2].has_rival = 1;
        arena->fighters[fighter2].rival_id = fighter1;

        printf("Организован бой: Боец %d vs Боец %d\n", fighter1, fighter2);
    }

    arena->round_num++;
    printf("Начало раунда %d. Бойцов готово к бою: %d\n", arena->round_num, count);

    sem_unlock(arena_sem);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Использовано %s <количество_бойцов>.\n", argv[0]);
        return 1;
    }

    int fighter_count = atoi(argv[1]);
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
    sem_unlink(SEM_NAME);

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

    arena = mmap(NULL, sizeof(Arena), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (arena == MAP_FAILED) {
        perror("Проблема с отображением памяти.");
        close(shm_fd);
        return 1;
    }

    memset(arena, 0, sizeof(Arena));
    arena->total_count = fighter_count;
    arena->alive_count = fighter_count;

    for (int i = 0; i < fighter_count; i++) {
        arena->fighters[i].id = i;
        arena->fighters[i].active = 1;
        arena->fighters[i].connected = 0;
        arena->fighters[i].victories = 0;
        arena->fighters[i].gesture = ROCK;
        arena->fighters[i].has_rival = 0;
        arena->fighters[i].rival_id = -1;
    }

    arena_sem = sem_open(SEM_NAME, O_CREAT, 0666, 1);
    if (arena_sem == SEM_FAILED) {
        perror("Проблема с созданием семафора.");
        cleanup_resources();
        return 1;
    }

    printf("Арена создана. Запустите процессы fighter:\n");
    for (int i = 0; i < fighter_count; i++) {
        printf("  ./fighter %d\n", i);
    }

    printf("\nОжидание подключения всех бойцов...\n");
    int wait_attempts = 30;
    int last_connected = -1;

    while (wait_attempts > 0) {
        int connected = get_connected_count();

        if (connected != last_connected) {
            printf("Подключено %d/%d бойцов\n", connected, fighter_count);
            last_connected = connected;
        }

        if (connected == fighter_count) {
            printf("Все бойцы подключены!\n");
            break;
        }
        sleep(1);
        wait_attempts--;
    }

    if (get_connected_count() != fighter_count) {
        printf("Не все бойцы подключились. Турнир отменен.\n");
        cleanup_resources();
        return 1;
    }

    printf("\n------ Турнир начинается! ------\n");
    sleep(2);

    int round = 0;
    while (!arena->finished) {
        sem_lock(arena_sem);
        int active = arena->alive_count;
        sem_unlock(arena_sem);

        if (active <= 1) {
            sem_lock(arena_sem);
            arena->finished = 1;
            sem_unlock(arena_sem);
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
            sem_lock(arena_sem);
            for (int i = 0; i < fighter_count; i++) {
                if (arena->fighters[i].has_rival) {
                    duels_active = 1;
                    break;
                }
            }
            sem_unlock(arena_sem);
            if (duels_active) {
                sleep(1);
                max_waits--;
            }
        } while (duels_active && max_waits > 0);

        print_active_fighters();
    }

    sem_lock(arena_sem);
    int winner_found = 0;
    for (int i = 0; i < fighter_count; i++) {
        if (arena->fighters[i].active) {
            printf("\nТурнир завершен! Победитель: Боец %d\n", i);
            winner_found = 1;
            break;
        }
    }

    if (!winner_found) {
        printf("\nТурнир завершен! Победитель не определен.\n");
    }
    sem_unlock(arena_sem);

    printf("Все бои завершены.\n");
    sleep(2);
    cleanup_resources();
    return 0;
}