#include <_time.h>
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

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
sem_t *watch_sem;
int observer_pipe = -1;
int observer_id;
char observer_pipe_path[64];

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

void observer_cleanup() {
    printf("Наблюдатель %d: освобождение ресурсов\n", observer_id);

    if (combat_zone) {
        munmap(combat_zone, sizeof(Arena));
    }
    if (zone_fd != -1) {
        close(zone_fd);
    }
    if (combat_sem != SEM_FAILED) {
        sem_close(combat_sem);
    }
    if (watch_sem != SEM_FAILED) {
        sem_close(watch_sem);
    }
    if (observer_pipe != -1) {
        close(observer_pipe);
    }
}

void signal_handler(int sig) {
    printf("Наблюдатель %d: останов по сигналу %d\n", observer_id, sig);
    observer_cleanup();
    exit(0);
}

const char* gesture_name(HandSign sign) {
    switch(sign) {
        case ROCK: return "Камень";
        case SCISSORS: return "Ножницы";
        case PAPER: return "Бумага";
        default: return "Неизвестно";
    }
}

int create_watch_channel(int watch_id) {
    snprintf(observer_pipe_path, sizeof(observer_pipe_path), "%s_%d", OBSERVER_PATH_BASE, watch_id);

    if (access(observer_pipe_path, F_OK) != -1) {
        printf("Канал %s уже существует\n", observer_pipe_path);
    } else {
        if (mkfifo(observer_pipe_path, 0666) == -1) {
            printf("Проблема с созданием канала наблюдателя: %s\n", strerror(errno));
            return -1;
        }
        printf("Создан канал: %s\n", observer_pipe_path);
    }

    observer_pipe = open(observer_pipe_path, O_RDONLY | O_NONBLOCK);
    if (observer_pipe == -1) {
        printf("Проблема с открытием канала наблюдателя: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

int get_duel_update(DuelMessage *msg) {
    if (observer_pipe == -1) return 0;

    int bytes_read = read(observer_pipe, msg, sizeof(DuelMessage));
    if (bytes_read == sizeof(DuelMessage)) {
        return 1;
    }
    return 0;
}

void show_full_status() {
    Arena zone_copy;
    int tournament_done = 0;
    int tournament_stopped = 0;

    sem_lock(combat_sem);
    memcpy(&zone_copy, combat_zone, sizeof(Arena));
    tournament_done = combat_zone->finished;
    tournament_stopped = combat_zone->terminated;
    sem_unlock(combat_sem);

    printf("\n=== НАБЛЮДАТЕЛЬ %d: СВОДКА ТУРНИРА ===\n", observer_id);
    printf("Текущий раунд: %d\n", zone_copy.round_num + 1);
    printf("Активных бойцов: %d/%d\n", zone_copy.alive_count, zone_copy.total_count);

    if (tournament_stopped) {
        printf("СТАТУС: ТУРНИР ПРЕРВАН\n");
    } else if (tournament_done) {
        printf("СТАТУС: ТУРНИР ЗАВЕРШЕН\n");
    } else {
        printf("СТАТУС: ТУРНИР АКТИВЕН\n");
    }

    printf("\nТекущие поединки:\n");
    int active_duels = 0;
    for (int i = 0; i < zone_copy.total_count; i++) {
        if (zone_copy.fighters[i].active &&
            zone_copy.fighters[i].has_rival &&
            zone_copy.fighters[i].rival_id > i) {
            printf("  Боец %d против Бойца %d\n", i, zone_copy.fighters[i].rival_id);
            active_duels++;
        }
    }
    if (active_duels == 0) {
        printf("  Активные поединки отсутствуют\n");
    }

    printf("\nСтатистика участников:\n");
    printf("ID | Статус      | Побед | Текущий жест\n");
    printf("---|-------------|-------|-------------\n");

    for (int i = 0; i < zone_copy.total_count; i++) {
        const char* status_text;
        if (!zone_copy.fighters[i].active) {
            status_text = "Выбыл";
        } else if (zone_copy.fighters[i].has_rival) {
            status_text = "В поединке";
        } else {
            status_text = "Ожидает";
        }

        printf("%2d | %-11s | %5d | %12s\n",
               i, status_text, zone_copy.fighters[i].victories,
               gesture_name(zone_copy.fighters[i].gesture));
    }

    if (tournament_done && !tournament_stopped) {
        for (int i = 0; i < zone_copy.total_count; i++) {
            if (zone_copy.fighters[i].active) {
                printf("\nПОБЕДИТЕЛЬ ТУРНИРА: Боец %d\n", i);
                break;
            }
        }
    } else if (tournament_stopped) {
        printf("\nТУРНИР БЫЛ ПРЕРВАН\n");
    }

    printf("====================================\n");
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
        printf("Использование: %s <ID_наблюдателя>\n", argv[0]);
        printf("ID наблюдателя должен быть от 0 до %d\n", MAX_OBSERVERS-1);
        return 1;
    }

    char *endptr;
    observer_id = strtol(argv[1], &endptr, 10);
    if (*endptr != '\0' || observer_id < 0 || observer_id >= MAX_OBSERVERS) {
        printf("Проблема: ID наблюдателя должен быть числом от 0 до %d\n", MAX_OBSERVERS-1);
        return 1;
    }

    printf("=== НАБЛЮДАТЕЛЬ %d ЗАПУЩЕН ===\n", observer_id);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (create_watch_channel(observer_id) == -1) {
        printf("Наблюдатель %d: проблема с созданием канала\n", observer_id);
        return 1;
    }

    if (!check_zone_exists()) {
        printf("Наблюдатель %d: арена не создана\n", observer_id);
        observer_cleanup();
        return 1;
    }

    zone_fd = shm_open("/battle_ground", O_RDWR, 0666);
    if (zone_fd == -1) {
        printf("Наблюдатель %d: проблема с подключением к арене: %s\n", observer_id, strerror(errno));
        observer_cleanup();
        return 1;
    }

    combat_zone = mmap(NULL, sizeof(Arena), PROT_READ | PROT_WRITE, MAP_SHARED, zone_fd, 0);
    if (combat_zone == MAP_FAILED) {
        printf("Наблюдатель %d: проблема с отображением памяти: %s\n", observer_id, strerror(errno));
        close(zone_fd);
        observer_cleanup();
        return 1;
    }

    combat_sem = sem_open("/battle_semaphore", 0);
    if (combat_sem == SEM_FAILED) {
        printf("Наблюдатель %d: проблема с подключением к семафору битвы: %s\n", observer_id, strerror(errno));
        observer_cleanup();
        return 1;
    }

    watch_sem = sem_open("/observer_semaphore", 0);
    if (watch_sem == SEM_FAILED) {
        printf("Наблюдатель %d: семафор наблюдателей недоступен: %s\n", observer_id, strerror(errno));
    }

    printf("Наблюдатель %d активирован\n", observer_id);
    printf("Канал: %s\n", observer_pipe_path);
    printf("Ожидание обновлений турнира\n\n");

    DuelMessage incoming_msg;
    int msg_count = 0;
    int empty_reads = 0;
    const int MAX_EMPTY = 100;

    while (1) {
        if (get_duel_update(&incoming_msg)) {
            empty_reads = 0;
            if (watch_sem != SEM_FAILED) {
                sem_lock(watch_sem);
            }
            printf("\n--- Наблюдатель %d ---\n", observer_id);
            printf("Обновление %d: %s\n", ++msg_count, incoming_msg.text);

            if (incoming_msg.is_result) {
                printf("Результат боя: %s против %s\n",
                       gesture_name(incoming_msg.move1),
                       gesture_name(incoming_msg.move2));
                printf("Количество раундов в бою: %d\n", incoming_msg.duel_rounds);
            }

            show_full_status();
            if (watch_sem != SEM_FAILED) {
                sem_unlock(watch_sem);
            }
        } else {
            empty_reads++;
            if (empty_reads > MAX_EMPTY) {
                empty_reads = 0;

                sem_lock(combat_sem);
                int should_exit = combat_zone->finished || combat_zone->terminated;
                sem_unlock(combat_sem);

                if (should_exit) {
                    break;
                }

                if (!check_zone_exists()) {
                    printf("Наблюдатель %d: арена уничтожена\n", observer_id);
                    break;
                }
            }
        }

        sem_lock(combat_sem);
        int should_exit = combat_zone->finished || combat_zone->terminated;
        sem_unlock(combat_sem);

        if (should_exit) {
            if (watch_sem != SEM_FAILED) {
                sem_lock(watch_sem);
            }
            printf("\n--- Наблюдатель %d: ФИНАЛЬНЫЙ ОТЧЕТ ---\n", observer_id);
            show_full_status();
            printf("\nНаблюдатель %d завершил мониторинг. Всего обновлений: %d\n",
                   observer_id, msg_count);
            if (watch_sem != SEM_FAILED) {
                sem_unlock(watch_sem);
            }
            break;
        }

        usleep(300000);
    }

    observer_cleanup();
    printf("Наблюдатель %d завершил работу\n", observer_id);
    return 0;
}