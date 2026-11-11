#include "sp.h"

#include <errno.h>
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

typedef struct NodeRoute {
  char *name;
  ChildNode *child;
  struct NodeRoute *next;
} NodeRoute;

enum {
  READ_LINE_OK = 0,
  READ_LINE_EOF = 1,
  READ_LINE_ERROR = -1
};

static Entry *table = NULL;
static char *node_name = NULL;
static char *program_path = NULL;
static int parent_command_fd = -1;
static int parent_reply_fd = -1;
static ChildNode *children = NULL;
static NodeRoute *routes = NULL;
static int is_child = 0;
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
static int parse_node_key_value(const char *line, size_t line_len,
                                const char *name, char **node, char **key,
                                char **value);
static int parse_node_key(const char *line, size_t line_len, const char *name,
                          char **node, char **key);
static int parse_create(const char *line, size_t line_len, char **parent,
                        char **child);
static ChildNode *find_child(const char *name);
static ChildNode *find_route(const char *name);
static int add_route(const char *name, ChildNode *child);
static int write_all(int fd, const char *data, size_t length);
static int write_line(int fd, const char *line);
static int read_line_from_fd(int fd, char **line, size_t *line_len);
static int spawn_child(const char *child_name);
static int dispatch_command(const Command *command);
static int parse_command_target(const Command *command, char **target);
static int run_loop(int input_fd);
static void free_children(void);
static void free_routes(void);

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

static int parse_node_key_value(const char *line, size_t line_len,
                                const char *name, char **node, char **key,
                                char **value) {
  size_t name_len = strlen(name);
  size_t node_start = name_len + 1;
  size_t node_end = node_start;
  size_t key_start;
  size_t key_end;

  *node = NULL;
  *key = NULL;
  *value = NULL;

  if (!command_name_matches(line, line_len, name) ||
      line_len <= node_start) {
    return -1;
  }

  while (node_end < line_len && line[node_end] != ' ') {
    node_end++;
  }
  if (node_end == node_start || node_end == line_len) {
    return -1;
  }

  key_start = node_end + 1;
  key_end = key_start;
  while (key_end < line_len && line[key_end] != ' ') {
    key_end++;
  }
  if (key_end == key_start || key_end == line_len) {
    return -1;
  }

  if (copy_range(line + node_start, node_end - node_start, node) != 0 ||
      copy_range(line + key_start, key_end - key_start, key) != 0 ||
      copy_range(line + key_end + 1, line_len - key_end - 1, value) != 0) {
    free(*node);
    free(*key);
    free(*value);
    *node = NULL;
    *key = NULL;
    *value = NULL;
    return -1;
  }

  if ((*value)[0] == '\0') {
    free(*node);
    free(*key);
    free(*value);
    *node = NULL;
    *key = NULL;
    *value = NULL;
    return -1;
  }

  return 0;
}

static int parse_node_key(const char *line, size_t line_len, const char *name,
                          char **node, char **key) {
  size_t name_len = strlen(name);
  size_t node_start = name_len + 1;
  size_t node_end = node_start;
  size_t key_start;

  *node = NULL;
  *key = NULL;

  if (!command_name_matches(line, line_len, name) ||
      line_len <= node_start) {
    return -1;
  }

  while (node_end < line_len && line[node_end] != ' ') {
    node_end++;
  }
  if (node_end == node_start || node_end == line_len) {
    return -1;
  }

  key_start = node_end + 1;
  if (key_start == line_len) {
    return -1;
  }

  for (size_t i = key_start; i < line_len; i++) {
    if (line[i] == ' ') {
      return -1;
    }
  }

  if (copy_range(line + node_start, node_end - node_start, node) != 0 ||
      copy_range(line + key_start, line_len - key_start, key) != 0) {
    free(*node);
    free(*key);
    *node = NULL;
    *key = NULL;
    return -1;
  }

  return 0;
}

static int parse_create(const char *line, size_t line_len, char **parent,
                        char **child) {
  return parse_two_keys(line, line_len, "create", parent, child);
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

static ChildNode *find_child(const char *name) {
  for (ChildNode *child = children; child != NULL; child = child->next) {
    if (strcmp(child->name, name) == 0) {
      return child;
    }
  }
  return NULL;
}

static ChildNode *find_route(const char *name) {
  for (NodeRoute *route = routes; route != NULL; route = route->next) {
    if (strcmp(route->name, name) == 0) {
      return route->child;
    }
  }
  return NULL;
}

static int add_route(const char *name, ChildNode *child) {
  NodeRoute *route = malloc(sizeof(*route));
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

static int write_all(int fd, const char *data, size_t length) {
  size_t written = 0;

  while (written < length) {
    ssize_t result = write(fd, data + written, length - written);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (result == 0) {
      return -1;
    }
    written += (size_t)result;
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
  size_t length = 0;
  size_t capacity = 0;
  char *data = NULL;

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

  if (length == SIZE_MAX) {
    free(data);
    return -1;
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

static int spawn_child(const char *child_name) {
  int command_pipe[2] = {-1, -1};
  int reply_pipe[2] = {-1, -1};

  if (pipe(command_pipe) != 0 || pipe(reply_pipe) != 0) {
    perror("pipe");
    if (command_pipe[0] >= 0) {
      close(command_pipe[0]);
      close(command_pipe[1]);
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
    close(reply_pipe[0]);
    close(reply_pipe[1]);
    return -1;
  }

  if (pid == 0) {
    char command_fd[32];
    char reply_fd[32];
    char *const arguments[] = {
        program_path, "--node", (char *)child_name, command_fd, reply_fd, NULL};

    for (ChildNode *existing = children; existing != NULL;
         existing = existing->next) {
      close(existing->command_fd);
      close(existing->reply_fd);
    }
    close(command_pipe[1]);
    close(reply_pipe[0]);
    snprintf(command_fd, sizeof(command_fd), "%d", command_pipe[0]);
    snprintf(reply_fd, sizeof(reply_fd), "%d", reply_pipe[1]);
    execv(program_path, arguments);
    perror("execv");
    _exit(127);
  }

  close(command_pipe[0]);
  close(reply_pipe[1]);

  char *ready = NULL;
  size_t ready_len = 0;
  int read_result = read_line_from_fd(reply_pipe[0], &ready, &ready_len);
  free(ready);
  if (read_result != 0) {
    fprintf(stderr, "Child '%s' failed to initialize\n", child_name);
    close(command_pipe[1]);
    close(reply_pipe[0]);
    (void)waitpid(pid, NULL, 0);
    return -1;
  }

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
  return 0;
}

static void free_children(void) {
  ChildNode *child = children;
  while (child != NULL) {
    ChildNode *next = child->next;
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
  NodeRoute *route = routes;
  while (route != NULL) {
    NodeRoute *next = route->next;
    free(route->name);
    free(route);
    route = next;
  }
  routes = NULL;
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

static int parse_command_target(const Command *command, char **target) {
  char *key = NULL;
  char *value = NULL;
  char *child = NULL;

  *target = NULL;
  switch (command->type) {
  case CREATE:
    if (parse_create(command->buf, command->len, target, &child) != 0) {
      free(child);
      return -1;
    }
    free(child);
    return 0;
  case STORE:
    if (parse_node_key_value(command->buf, command->len, "store", target,
                             &key, &value) != 0) {
      free(key);
      free(value);
      return -1;
    }
    free(key);
    free(value);
    return 0;
  case DELETE:
    if (parse_node_key(command->buf, command->len, "delete", target, &key) !=
        0) {
      free(key);
      return -1;
    }
    free(key);
    return 0;
  case FETCH:
    if (parse_node_key(command->buf, command->len, "fetch", target, &key) !=
        0) {
      free(key);
      return -1;
    }
    free(key);
    return 0;
  case KILL:
    if (parse_single_key(command->buf, command->len, "kill", target) == 0) {
      return 0;
    }
    return -1;
  default:
    return -1;
  }
}

static int dispatch_command(const Command *command) {
  char *target = NULL;
  if (parse_command_target(command, &target) != 0) {
    fprintf(stderr, "Invalid command\n");
    return -1;
  }

  if (strcmp(target, node_name) == 0) {
    int result = command_handler((Command *)command);
    free(target);
    return result;
  }

  ChildNode *child = find_route(target);
  if (child == NULL) {
    fprintf(stderr, "Unknown node '%s'\n", target);
    free(target);
    return -1;
  }

  if (write_line(child->command_fd, command->buf) != 0) {
    perror("write child command");
    free(target);
    return -1;
  }

  char *response = NULL;
  size_t response_len = 0;
  int read_result = read_line_from_fd(child->reply_fd, &response, &response_len);
  if (read_result != 0) {
    fprintf(stderr, "Child '%s' stopped responding\n", child->name);
    free(response);
    free(target);
    return -1;
  }

  if (command->type == CREATE && strcmp(response, "OK") == 0) {
    char *parent = NULL;
    char *new_child = NULL;
    if (parse_create(command->buf, command->len, &parent, &new_child) != 0 ||
        (find_route(new_child) == NULL &&
         add_route(new_child, child) != 0)) {
      free(parent);
      free(new_child);
      free(response);
      free(target);
      return -1;
    }
    free(parent);
    free(new_child);
  }

  if (is_child) {
    int result = write_line(parent_reply_fd, response);
    free(response);
    free(target);
    return result == 0 ? 0 : -1;
  }

  int result = 0;
  if (strcmp(response, "OK") == 0 || strcmp(response, "MISS") == 0) {
    result = strcmp(response, "MISS") == 0 ? 1 : 0;
  } else if (printf("%s\n", response) < 0 || fflush(stdout) != 0) {
    perror("write fetch reply");
    result = -1;
  }

  free(response);
  free(target);
  return result;
}

static int run_loop(int input_fd) {
  InputBuffer input = {0};
  int exit_status = 0;
  int running = 1;

  while (running) {
    char *command_buffer = NULL;
    size_t command_length = 0;
    int read_result =
        read_command_line(input_fd, &input, &command_buffer, &command_length);

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
    char *command_target = NULL;
    int target_result = parse_command_target(&command, &command_target);
    int is_local_command =
        target_result == 0 && strcmp(command_target, node_name) == 0;
    int handler_result = dispatch_command(&command);
    if (is_child && is_local_command &&
        (command.type != FETCH || handler_result < 0)) {
      if (write_line(parent_reply_fd,
                     handler_result < 0 ? "ERR" : "OK") != 0) {
        exit_status = 1;
        free(command_target);
        free(command_buffer);
        break;
      }
    }
    free(command_target);
    free(command_buffer);

    if (handler_result == 1 && command.type == KILL) {
      running = 0;
    }
  }

  free(input.data);
  return exit_status;
}

int main(int argc, char *argv[]) {
  program_path = argv[0];

  if (argc == 1) {
    node_name = duplicate_string("main");
    if (node_name == NULL) {
      perror("malloc");
      return 1;
    }
  } else if (argc == 5 && strcmp(argv[1], "--node") == 0) {
    char *end = NULL;
    long command_fd = strtol(argv[3], &end, 10);
    if (*argv[3] == '\0' || *end != '\0' || command_fd < 0) {
      fprintf(stderr, "Invalid child command fd\n");
      return 1;
    }
    long reply_fd = strtol(argv[4], &end, 10);
    if (*argv[4] == '\0' || *end != '\0' || reply_fd < 0) {
      fprintf(stderr, "Invalid child reply fd\n");
      return 1;
    }

    node_name = duplicate_string(argv[2]);
    if (node_name == NULL) {
      perror("malloc");
      return 1;
    }
    parent_command_fd = (int)command_fd;
    parent_reply_fd = (int)reply_fd;
    is_child = 1;
    if (write_line(parent_reply_fd, "READY") != 0) {
      perror("write child ready");
      free(node_name);
      return 1;
    }
  } else {
    fprintf(stderr, "Usage: ./sp\n");
    return 1;
  }

  int exit_status = run_loop(is_child ? parent_command_fd : STDIN_FILENO);
  free_children();
  free_routes();
  free_table();
  if (is_child) {
    close(parent_command_fd);
    close(parent_reply_fd);
  }
  free(node_name);
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
  char *parent = NULL;
  char *child = NULL;

  if (parse_create(buf, len, &parent, &child) != 0 ||
      strcmp(parent, node_name) != 0 || find_child(child) != NULL) {
    fprintf(stderr, "Invalid create command\n");
    free(parent);
    free(child);
    return -1;
  }

  int result = spawn_child(child);
  free(parent);
  free(child);
  return result;
}

int store_handler(char *buf, size_t len) {
  char *node = NULL;
  char *key = NULL;
  char *value = NULL;

  if (parse_node_key_value(buf, len, "store", &node, &key, &value) != 0 ||
      strcmp(node, node_name) != 0) {
    fprintf(stderr, "Invalid store command\n");
    free(node);
    free(key);
    free(value);
    return -1;
  }

  int result = set_entry(key, value);
  if (result != 0) {
    perror("set_entry");
  }

  free(node);
  free(key);
  free(value);
  return result;
}

int delete_handler(char *buf, size_t len) {
  char *node = NULL;
  char *key = NULL;

  if (parse_node_key(buf, len, "delete", &node, &key) != 0 ||
      strcmp(node, node_name) != 0) {
    fprintf(stderr, "Invalid delete command\n");
    free(node);
    free(key);
    return -1;
  }

  int result = remove_entry(key);
  free(node);
  free(key);
  return result;
}

int fetch_handler(char *buf, size_t len) {
  char *node = NULL;
  char *key = NULL;

  if (parse_node_key(buf, len, "fetch", &node, &key) != 0 ||
      strcmp(node, node_name) != 0) {
    fprintf(stderr, "Invalid fetch command\n");
    free(node);
    free(key);
    return -1;
  }

  Entry *entry = find_entry(key);
  if (entry == NULL) {
    if (is_child && write_line(parent_reply_fd, "MISS") != 0) {
      perror("write fetch miss");
    }
    free(node);
    free(key);
    return 1;
  }

  if (is_child) {
    size_t node_len = strlen(node);
    size_t value_len = strlen(entry->value);
    if (node_len > SIZE_MAX - value_len - 2) {
      free(node);
      free(key);
      return -1;
    }
    char *reply = malloc(node_len + value_len + 2);
    if (reply == NULL) {
      free(node);
      free(key);
      return -1;
    }
    memcpy(reply, node, node_len);
    reply[node_len] = ' ';
    memcpy(reply + node_len + 1, entry->value, value_len + 1);
    int write_result = write_line(parent_reply_fd, reply);
    free(reply);
    if (write_result != 0) {
      perror("write fetch reply");
      free(node);
      free(key);
      return -1;
    }
  } else if (printf("%s %s\n", node, entry->value) < 0 ||
             fflush(stdout) != 0) {
    perror("write fetch reply");
    free(node);
    free(key);
    return -1;
  }

  free(node);
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