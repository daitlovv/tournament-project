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
sem_t *display_sem;
sem_t *watch_sem;

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

void cleanup_pipes() {
    for (int i = 0; i < MAX_OBSERVERS; i++) {
        char pipe_path[64];
        snprintf(pipe_path, sizeof(pipe_path), "%s_%d", OBSERVER_PATH_BASE, i);
        unlink(pipe_path);
    }
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
            close(pipe_fd);
        }
    }
}

void zone_cleanup() {
    cleanup_pipes();

    if (combat_zone) {
        munmap(combat_zone, sizeof(Arena));
    }
    if (zone_fd != -1) {
        close(zone_fd);
        shm_unlink("/battle_ground");
    }
    if (combat_sem != SEM_FAILED) {
        sem_close(combat_sem);
        sem_unlink("/battle_semaphore");
    }
    if (display_sem != SEM_FAILED) {
        sem_close(display_sem);
        sem_unlink("/display_semaphore");
    }
    if (watch_sem != SEM_FAILED) {
        sem_close(watch_sem);
        sem_unlink("/observer_semaphore");
    }
}

void signal_handler(int sig) {
    printf("Турнир остановлен по сигналу %d\n", sig);

    sem_lock(combat_sem);
    combat_zone->finished = 1;
    combat_zone->terminated = 1;
    sem_unlock(combat_sem);

    send_to_watchers("Турнир остановлен по сигналу", -1, -1, 0, ROCK, ROCK, 0);

    sleep(2);
    zone_cleanup();
    exit(0);
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

        char message[MSG_SIZE];
        snprintf(message, MSG_SIZE, "Организован бой: Боец %d vs Боец %d", fighter1, fighter2);
        send_to_watchers(message, -1, -1, 0, ROCK, ROCK, 0);
    }

    if (count % 2 == 1 && count > 1) {
        int lucky_fighter = ready_fighters[count - 1];
        char message[MSG_SIZE];
        snprintf(message, MSG_SIZE, "Боец %d проходит автоматически", lucky_fighter);
        send_to_watchers(message, -1, -1, 0, ROCK, ROCK, 0);
    }

    combat_zone->round_num++;

    char round_msg[MSG_SIZE];
    snprintf(round_msg, MSG_SIZE, "Начало раунда %d", combat_zone->round_num);
    send_to_watchers(round_msg, -1, -1, 0, ROCK, ROCK, 0);

    sem_unlock(combat_sem);
}

int check_fighters_ready() {
    sem_lock(combat_sem);
    int ready = 0;
    for (int i = 0; i < combat_zone->total_count; i++) {
        if (combat_zone->fighters[i].active && combat_zone->fighters[i].connected) {
            ready++;
        }
    }
    sem_unlock(combat_sem);
    return ready;
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

    cleanup_pipes();

    zone_fd = shm_open("/battle_ground", O_CREAT | O_RDWR, 0666);
    if (zone_fd == -1) {
        perror("Проблема создания разделяемой памяти");
        return 1;
    }

    if (ftruncate(zone_fd, sizeof(Arena)) == -1) {
        perror("Проблема установки размера разделяемой памяти");
        close(zone_fd);
        return 1;
    }

    combat_zone = mmap(NULL, sizeof(Arena), PROT_READ | PROT_WRITE, MAP_SHARED, zone_fd, 0);
    if (combat_zone == MAP_FAILED) {
        perror("Проблема отображения памяти");
        close(zone_fd);
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
        combat_zone->fighters[i].connected = 0;
        combat_zone->fighters[i].victories = 0;
        combat_zone->fighters[i].gesture = ROCK;
        combat_zone->fighters[i].has_rival = 0;
        combat_zone->fighters[i].rival_id = -1;
    }

    combat_sem = sem_open("/battle_semaphore", O_CREAT | O_EXCL, 0666, 1);
    if (combat_sem == SEM_FAILED) {
        if (errno == EEXIST) {
            sem_unlink("/battle_semaphore");
            combat_sem = sem_open("/battle_semaphore", O_CREAT, 0666, 1);
        }
        if (combat_sem == SEM_FAILED) {
            perror("Проблема создания семафора битвы");
            zone_cleanup();
            return 1;
        }
    }

    display_sem = sem_open("/display_semaphore", O_CREAT | O_EXCL, 0666, 1);
    if (display_sem == SEM_FAILED) {
        if (errno == EEXIST) {
            sem_unlink("/display_semaphore");
            display_sem = sem_open("/display_semaphore", O_CREAT, 0666, 1);
        }
        if (display_sem == SEM_FAILED) {
            perror("Проблема создания семафора отображения");
            zone_cleanup();
            return 1;
        }
    }

    watch_sem = sem_open("/observer_semaphore", O_CREAT | O_EXCL, 0666, 1);
    if (watch_sem == SEM_FAILED) {
        if (errno == EEXIST) {
            sem_unlink("/observer_semaphore");
            watch_sem = sem_open("/observer_semaphore", O_CREAT, 0666, 1);
        }
        if (watch_sem == SEM_FAILED) {
            perror("Проблема создания семафора наблюдателей");
        }
    }

    printf("Центр управления турниром запущен\n");
    printf("Количество участников: %d\n", fighter_count);
    printf("Запустите %d процессов fighter с идентификаторами от 0 до %d\n",
           fighter_count, fighter_count - 1);

    printf("Ожидание подключения бойцов\n");
    printf("У вас есть 60 секунд чтобы запустить бойцов.\n");
    sleep(60);

    send_to_watchers("Турнир начал работу", -1, -1, 0, ROCK, ROCK, 0);

    printf("Начинаем турнир!\n");

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

        sem_lock(display_sem);
        printf("Управление: Раунд %d, активных бойцов: %d\n", round + 1, active);
        sem_unlock(display_sem);

        setup_round();

        sleep(4);

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

        round++;
    }

    sem_lock(combat_sem);
    for (int i = 0; i < combat_zone->total_count; i++) {
        if (combat_zone->fighters[i].active) {
            sem_lock(display_sem);
            printf("Турнир завершен. Победитель: Боец %d\n", i);
            sem_unlock(display_sem);

            char winner_msg[MSG_SIZE];
            snprintf(winner_msg, MSG_SIZE, "Турнир завершен! Победитель: Боец %d", i);
            send_to_watchers(winner_msg, -1, -1, 0, ROCK, ROCK, 0);
            break;
        }
    }
    sem_unlock(combat_sem);

    printf("Все бои завершены\n");
    sleep(2);

    zone_cleanup();
    return 0;
}
