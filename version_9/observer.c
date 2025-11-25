#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>

#define PIPE_NAME "/tmp/tournament_observer_9"

int pipe_fd = -1;
volatile sig_atomic_t keep_running = 1;

void cleanup() {
  if (pipe_fd != -1) {
    close(pipe_fd);
  }
}

void signal_handler(int sig) {
  printf("\nНаблюдатель остановлен по сигналу %d.\n", sig);
  keep_running = 0;
}

int main() {
  printf("------ Наблюдатель турнира ------\n");

  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  int wait_attempts = 30;
  while (wait_attempts > 0 && keep_running) {
      if (access(PIPE_NAME, F_OK) != -1) {
          break;
      }
      printf("Ожидание создания турнира...\n");        
      sleep(10);
      wait_attempts--;
  }

  if (access(PIPE_NAME, F_OK) == -1) {
      printf("Турнир не запущен в течение данного времени.\n");
      return 1;
  }

  pipe_fd = open(PIPE_NAME, O_RDONLY);
  if (pipe_fd == -1) {
    printf("Проблема с подключением к турниру.\n");
    return 1;
  }

  printf("Ожидание событий турнира...\n\n");

  char buffer[256];
  int message_count = 0;
  int empty_reads = 0;

  while (keep_running) {
    int bytes_read = read(pipe_fd, buffer, sizeof(buffer) - 1);
    if (bytes_read > 0) {
      empty_reads = 0;
      buffer[bytes_read] = '\0';
      printf("[%d] %s\n", ++message_count, buffer);

      if (strstr(buffer, "Турнир завершен") != NULL) {
        printf("\n------ Турнир завершен ------\n");
        break;
      }
    } else {
      empty_reads++;
      if (empty_reads > 100) {
        printf("Турнир завершился.\n");
        break;
      }
      usleep(100000);
    }
  }

  cleanup();
  printf("Наблюдатель завершил работу. Событий: %d.\n", message_count);
  return 0;
}
