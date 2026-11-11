#include "sp.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

typedef struct ChildNode {
  char *name;
  pid_t pid;
  int command_fd;
  int reply_fd;
  struct ChildNode *next;
} ChildNode;

typedef struct Route {
  char *name;
  ChildNode *child;
  struct Route *next;
} Route;

enum {
  READ_LINE_OK = 0,
  READ_LINE_EOF = 1,
  READ_LINE_ERROR = -1
};

static Entry *table = NULL;
static const char *program_path = NULL;
static const char *current_node = "main";
static int child_mode = 0;
static int child_command_fd = -1;
static int child_reply_fd = -1;
static ChildNode *children = NULL;
static Route *routes = NULL;
static Entry *find_entry(const char *key);
static int set_entry(const char *key, const char *value);
static int remove_entry(const char *key);
static void free_table(void);
static int read_command_line(int fd, InputBuffer *input, char **line,
                             size_t *line_len);
CommandType classify_command(const char *line, size_t line_len);
static int parse_key_value(const char *line, size_t line_len, const char *name,
                           char **key, char **value);
static int parse_single_key(const char *line, size_t line_len,
                            const char *name, char **key);
static int parse_two_keys(const char *line, size_t line_len, const char *name,
                          char **first, char **second);
static int write_all(int fd, const char *data, size_t length);
static int write_line(int fd, const char *line);
static int read_line_from_fd(int fd, char **line, size_t *line_len);
static ChildNode *find_child(const char *name);
static ChildNode *find_route(const char *name);
static int add_route(const char *name, ChildNode *child);
static int spawn_child(const char *parent_name, const char *child_name);
static void free_children(void);
static void free_routes(void);
static int run_command_loop(int fd);
static int command_target(const Command *command, char **target);
static int normalize_local_command(const Command *command, char **buffer,
                                   size_t *length);
static int process_command(const Command *command);

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

static int write_all(int fd, const char *data, size_t length) {
  size_t offset = 0;

  while (offset < length) {
    ssize_t written = write(fd, data + offset, length - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (written == 0) {
      return -1;
    }
    offset += (size_t)written;
  }

  return 0;
}

static int write_line(int fd, const char *line) {
  size_t length = strlen(line);
  if (length == SIZE_MAX || write_all(fd, line, length) != 0 ||
      write_all(fd, "\n", 1) != 0) {
    return -1;
  }
  return 0;
}

static int read_line_from_fd(int fd, char **line, size_t *line_len) {
  char *data = NULL;
  size_t length = 0;
  size_t capacity = 0;

  *line = NULL;
  *line_len = 0;

  for (;;) {
    char character;
    ssize_t result = read(fd, &character, 1);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      free(data);
      return -1;
    }
    if (result == 0) {
      if (length == 0) {
        free(data);
        return 1;
      }
      break;
    }
    if (character == '\n') {
      break;
    }
    if (length == SIZE_MAX - 1) {
      free(data);
      return -1;
    }
    if (length + 1 >= capacity) {
      size_t new_capacity = capacity == 0 ? 4096 : capacity;
      while (new_capacity <= length + 1) {
        if (new_capacity > SIZE_MAX / 2) {
          new_capacity = length + 2;
          break;
        }
        new_capacity *= 2;
      }
      char *new_data = realloc(data, new_capacity);
      if (new_data == NULL) {
        free(data);
        return -1;
      }
      data = new_data;
      capacity = new_capacity;
    }
    data[length++] = character;
  }

  if (data == NULL) {
    data = malloc(1);
    if (data == NULL) {
      return -1;
    }
  }
  data[length] = '\0';
  *line = data;
  *line_len = length;
  return 0;
}

static ChildNode *find_child(const char *name) {
  for (ChildNode *child = children; child != NULL; child = child->next) {
    if (strcmp(child->name, name) == 0) {
      return child;
    }
  }
  return NULL;
}

static ChildNode *find_route(const char *name) {
  for (Route *route = routes; route != NULL; route = route->next) {
    if (strcmp(route->name, name) == 0) {
      return route->child;
    }
  }
  return NULL;
}

static int add_route(const char *name, ChildNode *child) {
  Route *route = malloc(sizeof(*route));
  if (route == NULL) {
    return -1;
  }
  route->name = duplicate_string(name);
  if (route->name == NULL) {
    free(route);
    return -1;
  }
  route->child = child;
  route->next = routes;
  routes = route;
  return 0;
}

static int spawn_child(const char *parent_name, const char *child_name) {
  int command_pipe[2] = {-1, -1};
  int reply_pipe[2] = {-1, -1};
  int ready_pipe[2] = {-1, -1};

  if (pipe(command_pipe) != 0 || pipe(reply_pipe) != 0 ||
      pipe(ready_pipe) != 0) {
    perror("pipe");
    if (command_pipe[0] >= 0) {
      close(command_pipe[0]);
      close(command_pipe[1]);
    }
    if (ready_pipe[0] >= 0) {
      close(ready_pipe[0]);
      close(ready_pipe[1]);
    }
    if (reply_pipe[0] >= 0) {
      close(reply_pipe[0]);
      close(reply_pipe[1]);
    }
    return -1;
  }

  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    close(command_pipe[0]);
    close(command_pipe[1]);
    close(ready_pipe[0]);
    close(ready_pipe[1]);
    return -1;
  }

  if (pid == 0) {
    char command_fd[32];
    char reply_fd[32];
    char ready_fd[32];
    char *const arguments[] = {
        (char *)program_path, "--node", (char *)child_name, command_fd,
        reply_fd, ready_fd, NULL};

    close(command_pipe[1]);
    close(reply_pipe[0]);
    close(ready_pipe[0]);
    snprintf(command_fd, sizeof(command_fd), "%d", command_pipe[0]);
    snprintf(reply_fd, sizeof(reply_fd), "%d", reply_pipe[1]);
    snprintf(ready_fd, sizeof(ready_fd), "%d", ready_pipe[1]);
    execvp(program_path, arguments);
    perror("execvp");
    _exit(127);
  }

  close(command_pipe[0]);
  close(reply_pipe[1]);
  close(ready_pipe[1]);

  char *ready = NULL;
  size_t ready_length = 0;
  int read_result = read_line_from_fd(ready_pipe[0], &ready, &ready_length);
  close(ready_pipe[0]);
  if (read_result != 0 || ready == NULL || strcmp(ready, "READY") != 0) {
    fprintf(stderr, "Child '%s' failed to initialize\n", child_name);
    free(ready);
    close(command_pipe[1]);
    close(reply_pipe[0]);
    (void)waitpid(pid, NULL, 0);
    return -1;
  }
  free(ready);

  ChildNode *child = malloc(sizeof(*child));
  if (child == NULL) {
    perror("malloc");
    close(command_pipe[1]);
    close(reply_pipe[0]);
    (void)waitpid(pid, NULL, 0);
    return -1;
  }
  child->name = duplicate_string(child_name);
  if (child->name == NULL) {
    free(child);
    close(command_pipe[1]);
    close(reply_pipe[0]);
    (void)waitpid(pid, NULL, 0);
    return -1;
  }
  child->pid = pid;
  child->command_fd = command_pipe[1];
  child->reply_fd = reply_pipe[0];
  child->next = children;
  children = child;
  if (add_route(child_name, child) != 0) {
    children = child->next;
    close(child->command_fd);
    close(child->reply_fd);
    (void)waitpid(child->pid, NULL, 0);
    free(child->name);
    free(child);
    return -1;
  }
  (void)parent_name;
  return 0;
}

static void free_children(void) {
  ChildNode *child = children;
  while (child != NULL) {
    ChildNode *next = child->next;
    (void)write_line(child->command_fd, "kill");
    close(child->command_fd);
    close(child->reply_fd);
    (void)waitpid(child->pid, NULL, 0);
    free(child->name);
    free(child);
    child = next;
  }
  children = NULL;
}

static void free_routes(void) {
  Route *route = routes;
  while (route != NULL) {
    Route *next = route->next;
    free(route->name);
    free(route);
    route = next;
  }
  routes = NULL;
}

static int command_target(const Command *command, char **target) {
  const char *line = command->buf;
  size_t name_length;
  size_t first_start;
  size_t first_end;
  size_t second_start;
  size_t second_end;

  *target = NULL;
  if (command->type == CREATE) {
    char *child = NULL;
    if (parse_two_keys(line, command->len, "create", target, &child) != 0) {
      free(child);
      return -1;
    }
    free(child);
    return 0;
  }

  if (command->type == FETCH) {
    char *key = NULL;
    if (parse_two_keys(line, command->len, "fetch", target, &key) != 0) {
      free(key);
      return -1;
    }
    free(key);
    return 0;
  }

  name_length = strlen(command->type == STORE
                           ? "store"
                           : command->type == DELETE ? "delete" : "kill");
  first_start = name_length + 1;
  if (command->len <= first_start) {
    *target = duplicate_string(current_node);
    return *target == NULL ? -1 : 0;
  }

  first_end = first_start;
  while (first_end < command->len && line[first_end] != ' ') {
    first_end++;
  }
  if (first_end == first_start) {
    return -1;
  }

  if (command->type == KILL) {
    if (first_end == command->len) {
      *target = duplicate_string(first_start == command->len ? current_node
                                                               : line + first_start);
    } else {
      *target = copy_range(line + first_start, first_end - first_start,
                           target) == 0
                    ? *target
                    : NULL;
    }
    return *target == NULL ? -1 : 0;
  }

  if (command->type == CREATE) {
    *target = copy_range(line + first_start, first_end - first_start,
                         target) == 0
                  ? *target
                  : NULL;
    return *target == NULL ? -1 : 0;
  }

  if (first_end == command->len) {
    *target = duplicate_string(current_node);
    return *target == NULL ? -1 : 0;
  }

  second_start = first_end + 1;
  second_end = second_start;
  while (second_end < command->len && line[second_end] != ' ') {
    second_end++;
  }

  if (command->type == DELETE) {
    *target = copy_range(line + first_start, first_end - first_start, target)
                  == 0
              ? *target
              : NULL;
  } else if (command->type == STORE && second_end < command->len) {
    *target = copy_range(line + first_start, first_end - first_start, target)
                  == 0
              ? *target
              : NULL;
  } else {
    *target = duplicate_string(current_node);
  }

  return *target == NULL ? -1 : 0;
}

static int normalize_local_command(const Command *command, char **buffer,
                                   size_t *length) {
  const char *line = command->buf;
  size_t name_length = strlen(command->type == STORE ? "store" : "delete");
  size_t first_start = name_length + 1;
  size_t first_end = first_start;
  size_t second_start;
  size_t second_end;

  *buffer = NULL;
  *length = 0;
  if (command->type != STORE && command->type != DELETE) {
    return copy_range(line, command->len, buffer) == 0
               ? (*length = command->len, 0)
               : -1;
  }

  while (first_end < command->len && line[first_end] != ' ') {
    first_end++;
  }
  if (first_end == command->len) {
    return copy_range(line, command->len, buffer) == 0
               ? (*length = command->len, 0)
               : -1;
  }

  second_start = first_end + 1;
  second_end = second_start;
  while (second_end < command->len && line[second_end] != ' ') {
    second_end++;
  }

  if ((command->type == DELETE && second_end == command->len) ||
      (command->type == STORE && second_end < command->len)) {
    size_t tail_length = command->len - second_end;
    size_t new_length = name_length + 1 + (second_end - second_start) +
                        (command->type == STORE ? tail_length : 0);
    char *normalized = malloc(new_length + 1);
    if (normalized == NULL) {
      return -1;
    }
    memcpy(normalized, line, name_length);
    normalized[name_length] = ' ';
    memcpy(normalized + name_length + 1, line + second_start,
           second_end - second_start);
    if (command->type == STORE) {
      memcpy(normalized + name_length + 1 + second_end - second_start,
             line + second_end, tail_length);
    }
    normalized[new_length] = '\0';
    *buffer = normalized;
    *length = new_length;
    return 0;
  }

  return copy_range(line, command->len, buffer) == 0
             ? (*length = command->len, 0)
             : -1;
}

static int process_command(const Command *command) {
  char *target = NULL;
  if (command_target(command, &target) != 0) {
    fprintf(stderr, "Invalid command target\n");
    return -1;
  }

  if (strcmp(target, current_node) != 0) {
    ChildNode *child = find_route(target);
    if (child == NULL || write_line(child->command_fd, command->buf) != 0) {
      free(target);
      return -1;
    }

    char *response = NULL;
    size_t response_length = 0;
    int read_result =
        read_line_from_fd(child->reply_fd, &response, &response_length);
    if (read_result != 0) {
      free(response);
      free(target);
      return -1;
    }
    if (command->type == CREATE && strncmp(response, "CREATED ", 8) == 0) {
      if (add_route(response + 8, child) != 0) {
        free(response);
        free(target);
        return -1;
      }
    }

    if (command->type == FETCH && strncmp(response, "MISS ", 5) == 0) {
      char *hash = NULL;
      char *key = NULL;
      if (parse_two_keys(command->buf, command->len, "fetch", &hash, &key) ==
          0) {
        Entry *entry = find_entry(key);
        if (entry != NULL) {
          size_t hash_length = strlen(hash);
          size_t value_length = strlen(entry->value);
          if (hash_length <= SIZE_MAX - value_length - 2) {
            char *ancestor_reply =
                malloc(hash_length + value_length + 2);
            if (ancestor_reply != NULL) {
              memcpy(ancestor_reply, hash, hash_length);
              ancestor_reply[hash_length] = ' ';
              memcpy(ancestor_reply + hash_length + 1, entry->value,
                     value_length + 1);
              free(response);
              response = ancestor_reply;
            }
          }
        }
      }
      free(hash);
      free(key);
    }

    if (child_mode) {
      int result = write_line(child_reply_fd, response);
      free(response);
      free(target);
      return result == 0 ? 0 : -1;
    }
    int result = 0;
    if (command->type == CREATE &&
        strncmp(response, "CREATED ", 8) == 0) {
      result = 0;
    } else if (strcmp(response, "ERR") == 0) {
      result = -1;
    } else if (strncmp(response, "MISS", 4) == 0 ||
               strcmp(response, "OK") == 0) {
      result = strncmp(response, "MISS", 4) == 0 ? 1 : 0;
    } else if (printf("%s\n", response) < 0 || fflush(stdout) != 0) {
      result = -1;
    }
    free(response);
    free(target);
    return result;
  }

  char *local_buffer = NULL;
  size_t local_length = 0;
  if (normalize_local_command(command, &local_buffer, &local_length) != 0) {
    free(target);
    return -1;
  }
  Command local_command = *command;
  local_command.buf = local_buffer;
  local_command.len = local_length;
  int result = command_handler(&local_command);
  free(local_buffer);

  if (child_mode && command->type != FETCH) {
    char *reply = NULL;
    if (result == 0 && command->type == CREATE) {
      char *parent = NULL;
      char *child = NULL;
      if (parse_two_keys(command->buf, command->len, "create", &parent,
                         &child) == 0) {
        size_t child_length = strlen(child);
        reply = malloc(child_length + 9);
        if (reply != NULL) {
          memcpy(reply, "CREATED ", 8);
          memcpy(reply + 8, child, child_length + 1);
        }
      }
      free(parent);
      free(child);
    }
    if (reply == NULL) {
      reply = duplicate_string(result < 0 ? "ERR" : "OK");
    }
    if (reply == NULL || write_line(child_reply_fd, reply) != 0) {
      result = -1;
    }
    free(reply);
  }
  free(target);
  return result;
}

static int run_command_loop(int fd) {
  InputBuffer input = {0};
  int exit_status = 0;
  int running = 1;

  while (running) {
    char *command_buffer = NULL;
    size_t command_length = 0;
    int read_result =
        read_command_line(fd, &input, &command_buffer, &command_length);

    if (read_result == READ_LINE_EOF) {
      break;
    }
    if (read_result == READ_LINE_ERROR) {
      exit_status = 1;
      break;
    }

    Command command = {
        .buf = command_buffer,
        .len = command_length,
        .type = classify_command(command_buffer, command_length),
    };
    int handler_result = process_command(&command);
    free(command_buffer);

    if (handler_result == 1 && command.type == KILL) {
      running = 0;
    }
  }

  free(input.data);
  return exit_status;
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

  if (argc == 1) {
    current_node = "main";
  } else if (argc == 6 && strcmp(argv[1], "--node") == 0) {
    char *end = NULL;
    long command_fd = strtol(argv[3], &end, 10);
    if (*argv[3] == '\0' || *end != '\0' || command_fd < 0 ||
        command_fd > INT_MAX) {
      fprintf(stderr, "Invalid child command fd\n");
      return 1;
    }

    long reply_fd = strtol(argv[4], &end, 10);
    if (*argv[4] == '\0' || *end != '\0' || reply_fd < 0 ||
        reply_fd > INT_MAX) {
      fprintf(stderr, "Invalid child reply fd\n");
      return 1;
    }

    long ready_fd = strtol(argv[5], &end, 10);
    if (*argv[5] == '\0' || *end != '\0' || ready_fd < 0 ||
        ready_fd > INT_MAX) {
      fprintf(stderr, "Invalid child ready fd\n");
      return 1;
    }

    current_node = argv[2];
    child_mode = 1;
    child_command_fd = (int)command_fd;
    child_reply_fd = (int)reply_fd;
    if (write_line((int)ready_fd, "READY") != 0) {
      perror("write child ready");
      close(child_command_fd);
      close((int)ready_fd);
      return 1;
    }
    close((int)ready_fd);
  } else {
    fprintf(stderr, "Usage: ./sp\n");
    return 1;
  }

  int exit_status = run_command_loop(child_mode ? child_command_fd
                                                : STDIN_FILENO);
  if (child_mode) {
    close(child_command_fd);
    close(child_reply_fd);
  }
  free_children();
  free_routes();
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
  char *parent_name = NULL;
  char *child_name = NULL;

  if (parse_two_keys(buf, len, "create", &parent_name, &child_name) != 0 ||
      strcmp(parent_name, current_node) != 0 ||
      find_child(child_name) != NULL) {
    fprintf(stderr, "Invalid create command\n");
    free(parent_name);
    free(child_name);
    return -1;
  }

  int result = spawn_child(parent_name, child_name);
  free(parent_name);
  free(child_name);
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
    if (child_mode && write_line(child_reply_fd, "MISS") != 0) {
      perror("write fetch miss");
      free(hash);
      free(key);
      return -1;
    }
    free(hash);
    free(key);
    return 1;
  }

  if (child_mode) {
    size_t hash_length = strlen(hash);
    size_t value_length = strlen(entry->value);
    if (hash_length > SIZE_MAX - value_length - 2) {
      free(hash);
      free(key);
      return -1;
    }
    char *reply = malloc(hash_length + value_length + 2);
    if (reply == NULL) {
      free(hash);
      free(key);
      return -1;
    }
    memcpy(reply, hash, hash_length);
    reply[hash_length] = ' ';
    memcpy(reply + hash_length + 1, entry->value, value_length + 1);
    int write_result = write_line(child_reply_fd, reply);
    free(reply);
    if (write_result != 0) {
      perror("write fetch reply");
      free(hash);
      free(key);
      return -1;
    }
  } else if (printf("%s %s\n", hash, entry->value) < 0 ||
             fflush(stdout) != 0) {
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
