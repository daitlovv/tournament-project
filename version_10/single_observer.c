#include <time.h>
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
#define OBSERVER_ID 0
#define SHM_NAME "/battle_arena_10"

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

int observer_pipe = -1;
char observer_pipe_path[64];

const char* gesture_name(HandSign sign) {
    switch(sign) {
        case ROCK: return "Камень";
        case SCISSORS: return "Ножницы";
        case PAPER: return "Бумага";
        default: return "Неизвестно";
    }
}

int create_watch_channel() {
    char pipe_path[64];
    snprintf(pipe_path, sizeof(pipe_path), "%s_%d", OBSERVER_PATH_BASE, OBSERVER_ID);

    if (access(pipe_path, F_OK) != -1) {
        unlink(pipe_path);
    }

    if (mkfifo(pipe_path, 0666) == -1) {
        return -1;
    }

    strcpy(observer_pipe_path, pipe_path);
    return 0;
}

int get_duel_update(DuelMessage *msg) {
    if (observer_pipe == -1) {
        observer_pipe = open(observer_pipe_path, O_RDONLY | O_NONBLOCK);
        if (observer_pipe == -1) {
            return 0;
        }
    }

    int bytes_read = read(observer_pipe, msg, sizeof(DuelMessage));
    if (bytes_read == sizeof(DuelMessage)) {
        return 1;
    }
    return 0;
}

int check_tournament_finished() {
    int fd = shm_open("/battle_arena_10", O_RDONLY, 0666);
    if (fd == -1) {
        return 1;
    }
    close(fd);
    return 0;
}

void observer_cleanup() {
    if (observer_pipe != -1) {
        close(observer_pipe);
        observer_pipe = -1;
    }
    unlink(observer_pipe_path);
}

void signal_handler(int sig) {
    printf("\nНаблюдатель остановлен по сигналу %d.\n", sig);
    observer_cleanup();
    exit(0);
}

int main() {
    printf("------ Наблюдатель турнира ------\n");

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (create_watch_channel() == -1) {
        printf("Проблема с созданием канала.\n");
        return 1;
    }

    printf("Ожидание событий турнира...\n\n");

    DuelMessage incoming_msg;
    int msg_count = 0;
    int empty_reads = 0;
    int max_empty_reads = 1000;

    while (1) {
        if (get_duel_update(&incoming_msg)) {
            empty_reads = 0;

            printf("[%d] %s\n", ++msg_count, incoming_msg.text);

            if (incoming_msg.is_result) {
                printf("   %s vs %s\n",
                       gesture_name(incoming_msg.move1),
                       gesture_name(incoming_msg.move2));
            }

            if (strstr(incoming_msg.text, "Турнир завершен") != NULL) {
                printf("\n------ Турнир завершен ------\n");
                break;
            }
        } else {
            empty_reads++;

            if (check_tournament_finished()) {
                printf("\nТурнир завершен.\n");
                break;
            }

            if (empty_reads > max_empty_reads) {
                printf("Достигнут лимит ожидания.\n");
                break;
            }
            usleep(100000);
        }
    }

    observer_cleanup();
    printf("Наблюдатель завершил работу. Событий: %d.\n", msg_count);
    return 0;
}
