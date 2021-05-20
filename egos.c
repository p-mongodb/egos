#define _GNU_SOURCE
#define _POSIX_C_SOURCE 199309L
#include <fcntl.h>
#include <time.h>
#include <string.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>

typedef struct state {
  char *buf;
  int capacity;
  int pos;
  int done;
} state;

int init_state(state *st) {
  st->buf = malloc(16301);
  if (!st->buf) {
    return -1;
  }
  st->capacity = 16300;
  st->pos = 0;
  st->done = 0;
  return 0;
}

int forward(char stream, int fd, state *st) {
  ssize_t bytes_read, remaining;
  char *start_search, *start_print;
  char *ptr, *next_ptr = NULL;
  struct timespec timespec;
  struct tm tm;
  int time_initialized = 0;
  char time_buf[201];

  // Position where we start searching for a newline.
  // Assume the buffer currently does not contain one.
  start_search = st->buf + st->pos;
  // Position where we start printing. The beginning of the buffer.
  start_print = st->buf;
  
  remaining = st->capacity - st->pos;
  if (remaining == 0) {
    st->buf = realloc(st->buf, st->capacity * 2 + 1);
    if (!st->buf) {
      return -1;
    }
    remaining += st->capacity;
    st->capacity = st->capacity * 2;
    st->buf[st->capacity] = 0;
  }

  // Read up to remaining buffer capacity.
  // Request up to 100 bytes at a time to avoid buffering of output by
  // the kernel.
  if (remaining > 100) {
    remaining = 100;
  }
  bytes_read = read(fd, st->buf + st->pos, remaining);
  if (bytes_read <= 0) {
    st->done = 1;
    return 0;
  }
  
  // Buffer position indicates the end of the data in the buffer.
  st->pos += bytes_read;
  
  for (;;) {
    // Look for a newline from search start position.
    ptr = (char *) memchr(start_search, '\n', st->pos - (start_search - st->buf));
    if (ptr == NULL) {
      break;
    }
    
    // ptr contains a newline, which is the end of our current output.
    // Terminate the string.
    *ptr = 0;
    // Print the string. We removed the newline from the string.
    if (!time_initialized) {
      // TODO handle errors
      clock_gettime(CLOCK_REALTIME, &timespec);
      localtime_r(&timespec.tv_sec, &tm);
      if (!strftime(time_buf, 200, "%Y-%m-%dT%H:%M:%S", &tm)) {
        return -1;
      }
      time_initialized = 1;
    }
    printf("[egos:%c %s.%06ld] %s\n", stream, time_buf, timespec.tv_nsec / 1000, start_print);
    // Advance starting pointer to the beginning of next line.
    start_search = start_print = next_ptr = ptr + 1;
  }
  
  if (next_ptr) {
    size_t amount_to_move = st->pos - (next_ptr - st->buf);
    // We printed something, move the data back in the buffer.
    memmove(st->buf, next_ptr, amount_to_move);
    st->pos = amount_to_move;
  }
}

int main(int argc, char * const argv[]) {
  pid_t pid;
  va_list ap;
  int status;
  char *str;
  int out_fds[2];
  int err_fds[2];
  fd_set rfds;
  fd_set efds;
  int rv;
  state out_state, err_state;
  
  if (argc <= 1) {
    fputs("Usage: egos program-to-execute ...\n", stderr);
    exit(2);
  }
  
  if (setvbuf(stdin, NULL, _IONBF, 0)) {
    fputs("egos: Failed to unbuffer stdin\n", stderr);
    exit(2);
  }
  
  if (setvbuf(stdout, NULL, _IONBF, 0)) {
    fputs("egos: Failed to unbuffer stdout\n", stderr);
    exit(2);
  }
  
  if (setvbuf(stderr, NULL, _IONBF, 0)) {
    fputs("egos: Failed to unbuffer stderr\n", stderr);
    exit(2);
  }
  
  if (pipe(out_fds)) {
    fputs("egos: Failed to create stdout pipe\n", stderr);
    exit(2);
  }
  if (pipe(err_fds)) {
    fputs("egos: Failed to create stdout pipe\n", stderr);
    exit(2);
  }
  
  pid = fork();
  switch (pid) {
    case -1:
      perror("egos: Failed to fork child");
      exit(1);
    case 0:
      if (dup2(out_fds[1], 1) == -1) {
        fputs("egos: Failed to dup stdout\n", stderr);
        exit(2);
      }
      close(out_fds[1]);
      if (dup2(err_fds[1], 2) == -1) {
        fputs("egos: Failed to dup stdout\n", stderr);
        exit(2);
      }
      close(err_fds[1]);
      execvp(argv[1], argv + 1);
      puts("ok");
      if (asprintf(&str, "egos: Failed to exec child: %s", argv[1]) == -1) {
        fputs("egos: Out of memory\n", stderr);
        exit(3);
      }
      perror(str);
      free(str);
      exit(1);
    default:
      close(out_fds[1]);
      close(err_fds[1]);
      fcntl(out_fds[0], F_SETFL, O_NONBLOCK);
      fcntl(out_fds[1], F_SETFL, O_NONBLOCK);
      if (init_state(&out_state) == -1) {
        fputs("egos: Failed to init state\n", stderr);
        exit(2);
      }
      if (init_state(&err_state) == -1) {
        fputs("egos: Failed to init state\n", stderr);
        exit(2);
      }
      for (;;) {
        FD_ZERO(&rfds);
        FD_SET(out_fds[0], &rfds);
        FD_SET(err_fds[0], &rfds);
        FD_ZERO(&efds);
        FD_SET(out_fds[0], &efds);
        FD_SET(err_fds[0], &efds);
        // Assuming err_fds are larger than out_fds
        rv = select(err_fds[0] + 1, &rfds, NULL, &efds, NULL);
        if (rv == -1) {
          perror("select");
          exit(3);
        } else if (rv) {
          if (FD_ISSET(out_fds[0], &rfds)) {
            if (forward('O', out_fds[0], &out_state) == -1) {
              fputs("egos: Failed to forward output\n", stderr);
              exit(3);
            }
          }
          if (FD_ISSET(err_fds[0], &rfds)) {
            if (forward('E', err_fds[0], &err_state) == -1) {
              fputs("egos: Failed to forward error\n", stderr);
              exit(3);
            }
          }
          if (FD_ISSET(out_fds[0], &efds)) {
            out_state.done = 1;
          }
          if (FD_ISSET(err_fds[0], &efds)) {
            err_state.done = 1;
          }
          if (out_state.done && err_state.done) {
            break;
          }
        }
      }
      if (out_state.pos > 0) {
        out_state.buf[out_state.pos] = 0;
        puts(out_state.buf);
      }
      if (err_state.pos > 0) {
        err_state.buf[err_state.pos] = 0;
        puts(err_state.buf);
      }
      waitpid(pid, &status, 0);
      // TODO does WIFEXITED also need to be called?
      exit(WEXITSTATUS(status));
  }
}
