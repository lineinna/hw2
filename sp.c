#define _GNU_SOURCE
#include "sp.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ========================================================================= */
/* Dynamic Line Buffer for non-blocking stream parsing                       */
/* ========================================================================= */

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} LineBuffer;

static void lb_init(LineBuffer *lb) {
  lb->data = NULL;
  lb->len = 0;
  lb->cap = 0;
}

static void lb_free(LineBuffer *lb) {
  free(lb->data);
  lb->data = NULL;
  lb->len = 0;
  lb->cap = 0;
}

static int lb_read_from_fd(LineBuffer *lb, int fd, int *eof_out) {
  char chunk[4096];
  *eof_out = 0;

  for (;;) {
    ssize_t n = read(fd, chunk, sizeof(chunk));
    if (n > 0) {
      if (lb->len + (size_t)n > lb->cap) {
        size_t new_cap = lb->cap ? lb->cap * 2 : 1024;
        while (new_cap < lb->len + (size_t)n) {
          if (new_cap > SIZE_MAX / 2) {
            new_cap = lb->len + (size_t)n;
            break;
          }
          new_cap *= 2;
        }
        char *p = realloc(lb->data, new_cap);
        if (p == NULL) {
          return -1;
        }
        lb->data = p;
        lb->cap = new_cap;
      }
      memcpy(lb->data + lb->len, chunk, (size_t)n);
      lb->len += (size_t)n;
    } else if (n == 0) {
      *eof_out = 1;
      return 0;
    } else {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0;
      }
      *eof_out = 1;
      return -1;
    }
  }
}

static int lb_pop_line(LineBuffer *lb, char **line_out, size_t *len_out) {
  *line_out = NULL;
  *len_out = 0;

  if (lb->len == 0) {
    return 0;
  }

  char *nl = memchr(lb->data, '\n', lb->len);
  if (nl == NULL) {
    return 0;
  }

  size_t line_len = (size_t)(nl - lb->data);
  char *line = malloc(line_len + 1);
  if (line == NULL) {
    return -1;
  }

  memcpy(line, lb->data, line_len);
  line[line_len] = '\0';

  size_t consumed = line_len + 1;
  size_t remaining = lb->len - consumed;
  if (remaining > 0) {
    memmove(lb->data, lb->data + consumed, remaining);
  }
  lb->len = remaining;

  *line_out = line;
  *len_out = line_len;
  return 1;
}

/* ========================================================================= */
/* Key-Value Storage                                                         */
/* ========================================================================= */

typedef struct Entry {
  char *key;
  char *value;
  struct Entry *next;
} Entry;

static Entry *table = NULL;

static char *duplicate_string(const char *source) {
  size_t length = strlen(source);
  char *copy = malloc(length + 1);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, source, length + 1);
  return copy;
}

static Entry *find_entry(const char *key) {
  for (Entry *entry = table; entry != NULL; entry = entry->next) {
    if (strcmp(entry->key, key) == 0) {
      return entry;
    }
  }
  return NULL;
}

static int set_entry(const char *key, const char *value) {
  Entry *entry = find_entry(key);
  if (entry != NULL) {
    char *new_value = duplicate_string(value);
    if (new_value == NULL) {
      return -1;
    }
    free(entry->value);
    entry->value = new_value;
    return 0;
  }

  Entry *new_entry = malloc(sizeof(*new_entry));
  if (new_entry == NULL) {
    return -1;
  }
  new_entry->key = duplicate_string(key);
  if (new_entry->key == NULL) {
    free(new_entry);
    return -1;
  }
  new_entry->value = duplicate_string(value);
  if (new_entry->value == NULL) {
    free(new_entry->key);
    free(new_entry);
    return -1;
  }
  new_entry->next = table;
  table = new_entry;
  return 0;
}

static int remove_entry(const char *key) {
  Entry *previous = NULL;
  Entry *entry = table;
  while (entry != NULL) {
    if (strcmp(entry->key, key) == 0) {
      if (previous == NULL) {
        table = entry->next;
      } else {
        previous->next = entry->next;
      }

      free(entry->key);
      free(entry->value);
      free(entry);
      return 0;
    }
    previous = entry;
    entry = entry->next;
  }
  return 0;
}

static void free_table(void) {
  Entry *entry = table;
  while (entry != NULL) {
    Entry *next = entry->next;
    free(entry->key);
    free(entry->value);
    free(entry);
    entry = next;
  }
  table = NULL;
}

/* ========================================================================= */
/* Command Classification and Parsing                                        */
/* ========================================================================= */

static int command_name_matches(const char *line, size_t line_len,
                                const char *name) {
  size_t name_len = strlen(name);
  if (line_len < name_len || memcmp(line, name, name_len) != 0) {
    return 0;
  }
  return line_len == name_len || line[name_len] == ' ';
}

CommandType classify_command(const char *line, size_t line_len) {
  if (command_name_matches(line, line_len, "create")) {
    return CREATE;
  }
  if (command_name_matches(line, line_len, "store")) {
    return STORE;
  }
  if (command_name_matches(line, line_len, "delete")) {
    return DELETE;
  }
  if (command_name_matches(line, line_len, "fetch")) {
    return FETCH;
  }
  if (command_name_matches(line, line_len, "kill")) {
    return KILL;
  }
  if (command_name_matches(line, line_len, "update")) {
    return UPDATE;
  }
  return UNKNOWN;
}

static int copy_range(const char *source, size_t length, char **result) {
  *result = malloc(length + 1);
  if (*result == NULL) {
    return -1;
  }
  memcpy(*result, source, length);
  (*result)[length] = '\0';
  return 0;
}

static int parse_key_value(const char *line, size_t line_len, const char *name,
                           char **key, char **value) {
  size_t name_len = strlen(name);
  size_t key_start = name_len + 1;
  size_t key_end;

  *key = NULL;
  *value = NULL;

  if (!command_name_matches(line, line_len, name) || line_len <= key_start) {
    return -1;
  }

  key_end = key_start;
  while (key_end < line_len && line[key_end] != ' ') {
    key_end++;
  }

  if (key_end == key_start || key_end == line_len) {
    return -1;
  }

  if (copy_range(line + key_start, key_end - key_start, key) != 0) {
    return -1;
  }

  if (copy_range(line + key_end + 1, line_len - key_end - 1, value) != 0) {
    free(*key);
    *key = NULL;
    return -1;
  }

  if ((*value)[0] == '\0') {
    free(*key);
    free(*value);
    *key = NULL;
    *value = NULL;
    return -1;
  }

  return 0;
}

static int parse_single_key(const char *line, size_t line_len, const char *name,
                            char **key) {
  size_t name_len = strlen(name);
  size_t key_start = name_len + 1;

  *key = NULL;

  if (!command_name_matches(line, line_len, name) || line_len <= key_start) {
    return -1;
  }

  for (size_t i = key_start; i < line_len; i++) {
    if (line[i] == ' ') {
      return -1;
    }
  }

  return copy_range(line + key_start, line_len - key_start, key);
}

static int parse_two_keys(const char *line, size_t line_len, const char *name,
                          char **first, char **second) {
  size_t name_len = strlen(name);
  size_t first_start = name_len + 1;
  size_t first_end = first_start;
  size_t second_start;

  *first = NULL;
  *second = NULL;

  if (!command_name_matches(line, line_len, name) || line_len <= first_start) {
    return -1;
  }

  while (first_end < line_len && line[first_end] != ' ') {
    first_end++;
  }

  if (first_end == first_start || first_end == line_len) {
    return -1;
  }

  second_start = first_end + 1;
  if (second_start == line_len) {
    return -1;
  }

  for (size_t i = second_start; i < line_len; i++) {
    if (line[i] == ' ') {
      return -1;
    }
  }

  if (copy_range(line + first_start, first_end - first_start, first) != 0) {
    return -1;
  }

  if (copy_range(line + second_start, line_len - second_start, second) != 0) {
    free(*first);
    *first = NULL;
    return -1;
  }

  return 0;
}

/* ========================================================================= */
/* Command Handlers (Stage A: Single Node)                                    */
/* ========================================================================= */

static int store_handler(const char *buf, size_t len) {
  char *key = NULL;
  char *value = NULL;

  if (parse_key_value(buf, len, "store", &key, &value) != 0) {
    fprintf(stderr, "Invalid store command\n");
    return -1;
  }

  int result = set_entry(key, value);
  free(key);
  free(value);
  return result;
}

static int delete_handler(const char *buf, size_t len) {
  char *key = NULL;

  if (parse_single_key(buf, len, "delete", &key) != 0) {
    fprintf(stderr, "Invalid delete command\n");
    return -1;
  }

  int result = remove_entry(key);
  free(key);
  return result;
}

static int fetch_handler(const char *buf, size_t len) {
  char *hash = NULL;
  char *key = NULL;

  if (parse_two_keys(buf, len, "fetch", &hash, &key) != 0) {
    fprintf(stderr, "Invalid fetch command\n");
    return -1;
  }

  Entry *entry = find_entry(key);
  if (entry == NULL) {
    /* In later stages, miss will query parent / consultant.
       In Stage A (single node), keys in test cases exist. */
    free(hash);
    free(key);
    return 1;
  }

  if (printf("%s %s\n", hash, entry->value) < 0 || fflush(stdout) != 0) {
    perror("write fetch reply");
    free(hash);
    free(key);
    return -1;
  }

  free(hash);
  free(key);
  return 0;
}

static int dispatch_command(const char *line, size_t len, int *should_terminate) {
  CommandType type = classify_command(line, len);
  switch (type) {
  case STORE:
    return store_handler(line, len);
  case DELETE:
    return delete_handler(line, len);
  case FETCH:
    return fetch_handler(line, len);
  case KILL:
    *should_terminate = 1;
    return 0;
  case CREATE:
  case UPDATE:
    /* Will be implemented in subsequent stages */
    return 0;
  default:
    fprintf(stderr, "Unknown command\n");
    return -1;
  }
}

static int set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return -1;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ========================================================================= */
/* Main Event Loop (Stage A)                                                 */
/* ========================================================================= */

int main(int argc, char *argv[]) {
  (void)argv;
  if (argc != 1) {
    fprintf(stderr, "Usage: ./sp\n");
    return 1;
  }

  signal(SIGPIPE, SIG_IGN);

  if (set_nonblocking(STDIN_FILENO) != 0) {
    perror("set_nonblocking stdin");
  }

  LineBuffer cmd_in_buf;
  lb_init(&cmd_in_buf);

  int is_terminating = 0;
  struct pollfd pfd;
  pfd.fd = STDIN_FILENO;
  pfd.events = POLLIN;

  while (!is_terminating) {
    pfd.revents = 0;
    int ret = poll(&pfd, 1, -1);
    if (ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("poll");
      break;
    }

    if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
      int eof = 0;
      lb_read_from_fd(&cmd_in_buf, STDIN_FILENO, &eof);

      char *line = NULL;
      size_t line_len = 0;
      while (lb_pop_line(&cmd_in_buf, &line, &line_len) == 1) {
        dispatch_command(line, line_len, &is_terminating);
        free(line);
        if (is_terminating) {
          break;
        }
      }

      if (eof) {
        /* EOF on stdin is treated as kill (spec §2.2.4) */
        is_terminating = 1;
      }
    }
  }

  lb_free(&cmd_in_buf);
  free_table();
  return 0;
}
