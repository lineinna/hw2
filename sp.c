#include "sp.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct Entry {
  char *key;
  char *value;
  struct Entry *next;
} Entry;

typedef struct {
  char *data;
  size_t len;
  size_t capacity;
} InputBuffer;

typedef struct Child {
  pid_t pid;
  char *command_fifo;
  char *reply_fifo;
  int to_child_fd;
  int from_child_fd;
  struct Child *next;
} Child;

enum {
  READ_LINE_OK = 0,
  READ_LINE_EOF = 1,
  READ_LINE_ERROR = -1
};

static Entry *table = NULL;
static Child *children = NULL;
static int child_mode = 0;
static int child_command_fd = -1;
static int child_reply_fd = -1;
static int parent_read_fd = -1;
static int parent_write_fd = -1;
static char *child_command_fifo = NULL;
static char *child_reply_fifo = NULL;
static volatile sig_atomic_t stopping = 0;
static const char *program_path = NULL;
static Entry *find_entry(const char *key);
static int set_entry(const char *key, const char *value);
static int remove_entry(const char *key);
static void free_table(void);
static void free_children(void);
static int spawn_child(const char *command_fifo, const char *reply_fifo);
static int read_command_line(int fd, InputBuffer *input, char **line,
                             size_t *line_len);
static int copy_range(const char *source, size_t length, char **result);
CommandType classify_command(const char *line, size_t line_len);
static int parse_key_value(const char *line, size_t line_len, const char *name,
                           char **key, char **value);
static int parse_single_key(const char *line, size_t line_len,
                            const char *name, char **key);
static int parse_two_keys(const char *line, size_t line_len, const char *name,
                          char **first, char **second);

static void stop_child(int signal_number) {
  (void)signal_number;
  stopping = 1;
}

static int install_child_signal_handler(void) {
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = stop_child;
  if (sigemptyset(&action.sa_mask) != 0 ||
      sigaction(SIGTERM, &action, NULL) != 0) {
    return -1;
  }
  return 0;
}

static int duplicate_fifo_path(const char *path, char **copy) {
  if (path[0] != '/') {
    return -1;
  }
  return copy_range(path, strlen(path), copy);
}

static int set_close_on_exec(int fd) {
  int flags = fcntl(fd, F_GETFD);
  if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
    return -1;
  }
  return 0;
}

static int protect_child_fds(void) {
  if (child_mode &&
      (set_close_on_exec(parent_read_fd) != 0 ||
       set_close_on_exec(parent_write_fd) != 0)) {
    return -1;
  }
  for (Child *child = children; child != NULL; child = child->next) {
    if (set_close_on_exec(child->to_child_fd) != 0 ||
        set_close_on_exec(child->from_child_fd) != 0) {
      return -1;
    }
  }
  return 0;
}

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

static char *duplicate_string(const char *source) {
  size_t length = strlen(source);
  if (length == SIZE_MAX) {
    return NULL;
  }
  char *copy = malloc(length + 1);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, source, length + 1);
  return copy;
}

static int copy_range(const char *source, size_t length, char **result) {
  if (length == SIZE_MAX) {
    return -1;
  }

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

  if (!command_name_matches(line, line_len, name) ||
      line_len <= key_start) {
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

static int parse_single_key(const char *line, size_t line_len,
                            const char *name, char **key) {
  size_t name_len = strlen(name);
  size_t key_start = name_len + 1;

  *key = NULL;

  if (!command_name_matches(line, line_len, name) ||
      line_len <= key_start) {
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

  if (!command_name_matches(line, line_len, name) ||
      line_len <= first_start) {
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

static void free_children(void) {
  Child *child = children;
  while (child != NULL) {
    Child *next = child->next;
    close(child->to_child_fd);
    close(child->from_child_fd);
    if (kill(child->pid, SIGTERM) != 0 && errno != ESRCH) {
      perror("kill child");
    }
    if (waitpid(child->pid, NULL, 0) < 0 && errno != ECHILD) {
      perror("waitpid child");
    }
    free(child->command_fifo);
    free(child->reply_fifo);
    free(child);
    child = next;
  }
  children = NULL;
}

static int add_child(pid_t pid, const char *command_fifo,
                     const char *reply_fifo, int to_child_fd,
                     int from_child_fd) {
  Child *child = malloc(sizeof(*child));
  if (child == NULL) {
    return -1;
  }
  child->pid = pid;
  child->command_fifo = NULL;
  child->reply_fifo = NULL;
  child->to_child_fd = to_child_fd;
  child->from_child_fd = from_child_fd;
  child->next = children;

  if (duplicate_fifo_path(command_fifo, &child->command_fifo) != 0 ||
      duplicate_fifo_path(reply_fifo, &child->reply_fifo) != 0) {
    free(child->command_fifo);
    free(child->reply_fifo);
    free(child);
    return -1;
  }

  children = child;
  return 0;
}

static int spawn_child(const char *command_fifo, const char *reply_fifo) {
  int to_child[2] = {-1, -1};
  int from_child[2] = {-1, -1};
  if (protect_child_fds() != 0) {
    perror("fcntl child fd");
    return -1;
  }
  if (pipe(to_child) != 0 || pipe(from_child) != 0) {
    perror("pipe");
    if (to_child[0] >= 0) {
      close(to_child[0]);
      close(to_child[1]);
    }
    if (from_child[0] >= 0) {
      close(from_child[0]);
      close(from_child[1]);
    }
    return -1;
  }

  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    close(to_child[0]);
    close(to_child[1]);
    close(from_child[0]);
    close(from_child[1]);
    return -1;
  }

  if (pid == 0) {
    char parent_read[32];
    char parent_write[32];
    (void)snprintf(parent_read, sizeof(parent_read), "%d", from_child[0]);
    (void)snprintf(parent_write, sizeof(parent_write), "%d", to_child[1]);
    close(to_child[0]);
    close(from_child[1]);
    char *const child_argv[] = {
        (char *)program_path, (char *)"--node", (char *)command_fifo,
        (char *)reply_fifo, parent_read, parent_write, NULL};
    execv(program_path, child_argv);
    perror("exec sp child");
    _exit(127);
  }

  close(to_child[1]);
  close(from_child[0]);
  if (set_close_on_exec(to_child[0]) != 0 ||
      set_close_on_exec(from_child[1]) != 0) {
    perror("fcntl child fd");
    close(to_child[0]);
    close(from_child[1]);
    if (kill(pid, SIGTERM) != 0 && errno != ESRCH) {
      perror("kill child");
    }
    (void)waitpid(pid, NULL, 0);
    return -1;
  }
  if (add_child(pid, command_fifo, reply_fifo, to_child[0],
                from_child[1]) != 0) {
    close(to_child[0]);
    close(from_child[1]);
    if (kill(pid, SIGTERM) != 0 && errno != ESRCH) {
      perror("kill child");
    }
    if (waitpid(pid, NULL, 0) < 0 && errno != ECHILD) {
      perror("waitpid child");
    }
    return -1;
  }
  return 0;
}

static int read_command_line(int fd, InputBuffer *input, char **line,
                             size_t *line_len) {
  char chunk[4096];

  *line = NULL;
  *line_len = 0;

  for (;;) {
    char *newline = memchr(input->data, '\n', input->len);
    if (newline != NULL) {
      size_t command_length = (size_t)(newline - input->data);
      size_t consumed = command_length + 1;
      size_t remaining = input->len - consumed;

      char *command = malloc(command_length + 1);
      if (command == NULL) {
        perror("malloc");
        return READ_LINE_ERROR;
      }

      memcpy(command, input->data, command_length);
      command[command_length] = '\0';

      memmove(input->data, input->data + consumed, remaining);
      input->len = remaining;

      *line = command;
      *line_len = command_length;
      return READ_LINE_OK;
    }

    ssize_t bytes_read = read(fd, chunk, sizeof(chunk));
    if (bytes_read < 0) {
      if (errno == EINTR) {
        if (stopping) {
          return READ_LINE_ERROR;
        }
        continue;
      }

      perror("read command");
      return READ_LINE_ERROR;
    }

    if (bytes_read == 0) {
      if (input->len == 0) {
        return READ_LINE_EOF;
      }

      char *command = malloc(input->len + 1);
      if (command == NULL) {
        perror("malloc");
        return READ_LINE_ERROR;
      }

      memcpy(command, input->data, input->len);
      command[input->len] = '\0';
      *line = command;
      *line_len = input->len;
      input->len = 0;
      return READ_LINE_OK;
    }

    size_t count = (size_t)bytes_read;
    if (input->len > SIZE_MAX - count) {
      fprintf(stderr, "Input buffer size overflow\n");
      return READ_LINE_ERROR;
    }

    size_t required = input->len + count;
    if (required > input->capacity) {
      size_t new_capacity = input->capacity == 0 ? sizeof(chunk)
                                                 : input->capacity;

      while (new_capacity < required) {
        if (new_capacity > SIZE_MAX / 2) {
          new_capacity = required;
          break;
        }
        new_capacity *= 2;
      }

      char *new_data = realloc(input->data, new_capacity);
      if (new_data == NULL) {
        perror("realloc");
        return READ_LINE_ERROR;
      }

      input->data = new_data;
      input->capacity = new_capacity;
    }

    memcpy(input->data + input->len, chunk, count);
    input->len += count;
  }
}

int main(int argc, char *argv[]) {
  program_path = argv[0];
  if (argc == 6 && strcmp(argv[1], "--node") == 0) {
    char *end = NULL;
    long parsed_read_fd = strtol(argv[4], &end, 10);
    if (*argv[4] == '\0' || *end != '\0' || parsed_read_fd < 0 ||
        parsed_read_fd > INT_MAX) {
      fprintf(stderr, "Invalid parent read fd\n");
      return 1;
    }
    long parsed_write_fd = strtol(argv[5], &end, 10);
    if (*argv[5] == '\0' || *end != '\0' || parsed_write_fd < 0 ||
        parsed_write_fd > INT_MAX) {
      fprintf(stderr, "Invalid parent write fd\n");
      return 1;
    }
    child_mode = 1;
    parent_read_fd = (int)parsed_read_fd;
    parent_write_fd = (int)parsed_write_fd;
    if (duplicate_fifo_path(argv[2], &child_command_fifo) != 0 ||
        duplicate_fifo_path(argv[3], &child_reply_fifo) != 0) {
      fprintf(stderr, "Invalid FIFO path\n");
      free(child_command_fifo);
      free(child_reply_fifo);
      return 1;
    }
    if (install_child_signal_handler() != 0) {
      perror("sigaction");
      free(child_command_fifo);
      free(child_reply_fifo);
      return 1;
    }
    if (mkfifo(child_command_fifo, 0600) != 0 ||
        mkfifo(child_reply_fifo, 0600) != 0) {
      perror("mkfifo");
      unlink(child_command_fifo);
      unlink(child_reply_fifo);
      free(child_command_fifo);
      free(child_reply_fifo);
      return 1;
    }
    child_command_fd = open(child_command_fifo, O_RDONLY);
    if (child_command_fd < 0) {
      perror("open command FIFO");
      unlink(child_command_fifo);
      unlink(child_reply_fifo);
      free(child_command_fifo);
      free(child_reply_fifo);
      return 1;
    }
    child_reply_fd = open(child_reply_fifo, O_WRONLY);
    if (child_reply_fd < 0) {
      perror("open reply FIFO");
      close(child_command_fd);
      unlink(child_command_fifo);
      unlink(child_reply_fifo);
      free(child_command_fifo);
      free(child_reply_fifo);
      return 1;
    }
  } else if (argc != 1) {
    fprintf(stderr, "Usage: ./sp\n");
    return 1;
  }

  InputBuffer input = {0};
  int exit_status = 0;
  int running = 1;

  while (running) {
    char *command_buffer = NULL;
    size_t command_length = 0;
    int read_result =
        read_command_line(child_mode ? child_command_fd : STDIN_FILENO, &input,
                          &command_buffer,
                          &command_length);

    if (read_result == READ_LINE_EOF) {
      break;
    }
    if (read_result == READ_LINE_ERROR) {
      if (stopping) {
        break;
      }
      exit_status = 1;
      break;
    }

    Command command = {
        .buf = command_buffer,
        .len = command_length,
        .type = classify_command(command_buffer, command_length),
    };
    int handler_result = command_handler(&command);
    free(command_buffer);

    if (handler_result == 1 && command.type == KILL) {
      running = 0;
    }
  }

  free(input.data);
  free_children();
  if (child_mode) {
    close(child_command_fd);
    close(child_reply_fd);
    close(parent_read_fd);
    close(parent_write_fd);
    unlink(child_command_fifo);
    unlink(child_reply_fifo);
    free(child_command_fifo);
    free(child_reply_fifo);
  }
  free_table();
  return exit_status;
}

int command_handler(Command *cmd) {
  switch (cmd->type) {
  case CREATE:
    return create_handler(cmd->buf, cmd->len);
  case STORE:
    return store_handler(cmd->buf, cmd->len);
  case DELETE:
    return delete_handler(cmd->buf, cmd->len);
  case FETCH:
    return fetch_handler(cmd->buf, cmd->len);
  case KILL:
    return kill_handler(cmd->buf, cmd->len);
  case UPDATE:
    return update_handler(cmd->buf, cmd->len);
  default:
    fprintf(stderr, "Unknown command type\n");
    return -1;
  }
}

int create_handler(char *buf, size_t len) {
  char *command_fifo = NULL;
  char *reply_fifo = NULL;

  if (parse_two_keys(buf, len, "create", &command_fifo, &reply_fifo) != 0) {
    fprintf(stderr, "Invalid create command\n");
    free(command_fifo);
    free(reply_fifo);
    return -1;
  }

  int result = spawn_child(command_fifo, reply_fifo);
  free(command_fifo);
  free(reply_fifo);
  return result;
}

int store_handler(char *buf, size_t len) {
  char *key = NULL;
  char *value = NULL;

  if (parse_key_value(buf, len, "store", &key, &value) != 0) {
    fprintf(stderr, "Invalid store command\n");
    return -1;
  }

  int result = set_entry(key, value);
  if (result != 0) {
    perror("set_entry");
  }

  free(key);
  free(value);
  return result;
}

int delete_handler(char *buf, size_t len) {
  char *key = NULL;

  if (parse_single_key(buf, len, "delete", &key) != 0) {
    fprintf(stderr, "Invalid delete command\n");
    return -1;
  }

  int result = remove_entry(key);
  free(key);
  return result;
}

int fetch_handler(char *buf, size_t len) {
  char *hash = NULL;
  char *key = NULL;

  if (parse_two_keys(buf, len, "fetch", &hash, &key) != 0) {
    fprintf(stderr, "Invalid fetch command\n");
    return -1;
  }

  Entry *entry = find_entry(key);
  if (entry == NULL) {
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

int kill_handler(char *buf, size_t len) {
  (void)buf;
  (void)len;
  free_table();
  return 1;
}

int update_handler(char *buf, size_t len) {
  // TODO: Implement the UPDATE command handler.
  (void)buf;
  (void)len;
  return -1;
}
