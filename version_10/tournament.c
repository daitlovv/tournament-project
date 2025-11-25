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
#define MSG_SIZE 256
#define OBSERVER_PATH_BASE "/tmp/battle_observer_10"
#define MAX_OBSERVERS 10
#define SHM_NAME "/battle_arena_10"
#define SEM_NAME "/battle_sem_10"

typedef enum {
    ROCK = 0,
    SCISSORS = 1,
    PAPER = 2
} HandSign;

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
    int terminated;
} Arena;

Arena *combat_zone;
int zone_fd;
sem_t *combat_sem;

void create_observer_channels() {
    for (int i = 0; i < MAX_OBSERVERS; i++) {
        char pipe_path[64];
        snprintf(pipe_path, sizeof(pipe_path), "%s_%d", OBSERVER_PATH_BASE, i);
        if (access(pipe_path, F_OK) != -1) {
            unlink(pipe_path);
        }
        mkfifo(pipe_path, 0666);
    }
}

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
            (void)written;
            close(pipe_fd);
        }
    }
}

void cleanup_resources() {
    printf("Очистка ресурсов.\n");
    if (combat_zone) {
        munmap(combat_zone, sizeof(Arena));
    }
    if (zone_fd != -1) {
        close(zone_fd);
        shm_unlink(SHM_NAME);
    }
    if (combat_sem != SEM_FAILED) {
        sem_close(combat_sem);
        sem_unlink(SEM_NAME);
    }

    for (int i = 0; i < MAX_OBSERVERS; i++) {
        char pipe_path[64];
        snprintf(pipe_path, sizeof(pipe_path), "%s_%d", OBSERVER_PATH_BASE, i);
        unlink(pipe_path);
    }
}

void signal_handler(int sig) {
    printf("Турнир остановлен по сигналу %d.\n", sig);
    sem_lock(combat_sem);
    combat_zone->finished = 1;
    combat_zone->terminated = 1;
    sem_unlock(combat_sem);
    send_to_watchers("Турнир остановлен по сигналу.", -1, -1, 0, ROCK, ROCK, 0);
    sleep(1);
    cleanup_resources();
    exit(0);
}

int get_connected_count() {
    sem_lock(combat_sem);
    int count = 0;
    for (int i = 0; i < combat_zone->total_count; i++) {
        if (combat_zone->fighters[i].connected) {
            count++;
        }
    }
    sem_unlock(combat_sem);
    return count;
}

void print_active_fighters() {
    sem_lock(combat_sem);
    printf("Промежуточные победители: ");
    int first = 1;
    for (int i = 0; i < combat_zone->total_count; i++) {
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

        printf("Организован бой:\n Боец %d vs Боец %d\n", fighter1, fighter2);
        char message[MSG_SIZE];
        snprintf(message, MSG_SIZE, "Организован бой: Боец %d vs Боец %d", fighter1, fighter2);
        send_to_watchers(message, -1, -1, 0, ROCK, ROCK, 0);
    }

    combat_zone->round_num++;
    printf("Начало раунда %d. Бойцов готово к бою: %d\n", combat_zone->round_num, count);

    char round_msg[MSG_SIZE];
    snprintf(round_msg, MSG_SIZE, "Начало раунда %d.", combat_zone->round_num);
    send_to_watchers(round_msg, -1, -1, 0, ROCK, ROCK, 0);

    sem_unlock(combat_sem);
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

    zone_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (zone_fd == -1) {
        perror("Проблема с созданием разделяемой памяти.");
        return 1;
    }

    if (ftruncate(zone_fd, sizeof(Arena)) == -1) {
        perror("Проблема с установкой размера памяти.");
        close(zone_fd);
        return 1;
    }

    combat_zone = mmap(NULL, sizeof(Arena), PROT_READ | PROT_WRITE, MAP_SHARED, zone_fd, 0);
    if (combat_zone == MAP_FAILED) {
        perror("Проблема с отображением памяти.");
        close(zone_fd);
        return 1;
    }

    memset(combat_zone, 0, sizeof(Arena));
    combat_zone->total_count = fighter_count;
    combat_zone->alive_count = fighter_count;

    for (int i = 0; i < fighter_count; i++) {
        combat_zone->fighters[i].id = i;
        combat_zone->fighters[i].active = 1;
        combat_zone->fighters[i].connected = 0;
        combat_zone->fighters[i].victories = 0;
        combat_zone->fighters[i].gesture = ROCK;
        combat_zone->fighters[i].has_rival = 0;
        combat_zone->fighters[i].rival_id = -1;
    }

    combat_sem = sem_open(SEM_NAME, O_CREAT, 0666, 1);
    if (combat_sem == SEM_FAILED) {
        perror("Проблема с созданием семафора.");
        cleanup_resources();
        return 1;
    }

    create_observer_channels();

    printf("Арена создана. Запустите процессы fighter:\n");
    for (int i = 0; i < fighter_count; i++) {
        printf("  ./fighter %d\n", i);
    }

    printf("\nОжидание подключения всех бойцов...\n");
    printf("У вас есть 60 секунд, чтобы подключить игроков.\n");
    int wait_attempts = 60;
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

    printf("\nЗапуск наблюдателей:\n");
    printf("Теперь у вас есть 40 секунд чтобы запустить наблюдателей.\n");
    send_to_watchers("Турнир начал работу.", -1, -1, 0, ROCK, ROCK, 0);
    sleep(40);

    printf("\n------ Турнир начинается! ------\n");
    send_to_watchers("Турнир начинается!", -1, -1, 0, ROCK, ROCK, 0);
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

        print_active_fighters();
    }

    sem_lock(combat_sem);
    int winner_found = 0;
    for (int i = 0; i < fighter_count; i++) {
        if (combat_zone->fighters[i].active) {
            printf("\nТурнир завершен! Победитель: Боец %d\n", i);
            char winner_msg[MSG_SIZE];
            snprintf(winner_msg, MSG_SIZE, "Турнир завершен! Победитель: Боец %d", i);
            send_to_watchers(winner_msg, -1, -1, 0, ROCK, ROCK, 0);
            winner_found = 1;
            break;
        }
    }
    
    if (!winner_found) {
        printf("\nТурнир завершен! Победитель не определен.\n");
        send_to_watchers("Турнир завершен! Победитель не определен.", -1, -1, 0, ROCK, ROCK, 0);
    }
    sem_unlock(combat_sem);

    printf("Все бои завершены.\n");
    send_to_watchers("Все бои завершены.", -1, -1, 0, ROCK, ROCK, 0);
    sleep(2);
    cleanup_resources();
    return 0;
}
