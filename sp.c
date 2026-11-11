#include "sp.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <poll.h>
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

typedef struct {
  char *data;
  size_t len;
  size_t capacity;
} ChannelBuffer;

typedef struct Child {
  pid_t pid;
  char *command_fifo;
  char *reply_fifo;
  int parent_write_fd;
  int parent_read_fd;
  ChannelBuffer input;
  struct Child *next;
} Child;

typedef struct PendingRequest {
  char *hash;
  char *key;
  Child *return_child;
  struct PendingRequest *next;
} PendingRequest;

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
static ChannelBuffer parent_input = {0};
static PendingRequest *pending_requests = NULL;
static char *child_command_fifo = NULL;
static char *child_reply_fifo = NULL;
static const char *program_path = NULL;
static volatile sig_atomic_t stopping = 0;
static Entry *find_entry(const char *key);
static int set_entry(const char *key, const char *value);
static int remove_entry(const char *key);
static void free_table(void);
static void free_children(void);
static int read_command_line(int fd, InputBuffer *input, char **line,
                             size_t *line_len);
static int copy_range(const char *source, size_t length, char **result);
static void free_channel_buffer(ChannelBuffer *buffer);
static int set_nonblocking(int fd);
static int append_channel_data(int fd, ChannelBuffer *buffer);
static int take_channel_line(ChannelBuffer *buffer, char **line);
static void free_pending_requests(void);
static int add_pending_request(const char *hash, const char *key,
                               Child *return_child);
static PendingRequest *find_pending_request(const char *hash);
static void remove_pending_request(PendingRequest *request);
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
  if (key_end == key_start || key_end == line_len ||
      copy_range(line + node_start, node_end - node_start, node) != 0 ||
      copy_range(line + key_start, key_end - key_start, key) != 0 ||
      copy_range(line + key_end + 1, line_len - key_end - 1, value) != 0 ||
      (*value)[0] == '\0') {
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
  if (key_start == line_len ||
      strchr(line + key_start, ' ') != NULL ||
      copy_range(line + node_start, node_end - node_start, node) != 0 ||
      copy_range(line + key_start, line_len - key_start, key) != 0) {
    free(*node);
    free(*key);
    *node = NULL;
    *key = NULL;
    return -1;
  }
  return 0;
}

static void stop_child(int signal_number) {
  (void)signal_number;
  stopping = 1;
}

static int set_close_on_exec(int fd) {
  int flags = fcntl(fd, F_GETFD);
  if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
    return -1;
  }
  return 0;
}

static int valid_fifo_path(const char *path) {
  return path[0] == '/' && strchr(path, ' ') == NULL &&
         strchr(path, '\t') == NULL;
}

static int write_all(int fd, const char *data, size_t length) {
  while (length > 0) {
    ssize_t written = write(fd, data, length);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      return -1;
    }
    data += written;
    length -= (size_t)written;
  }
  return 0;
}

static int set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    return -1;
  }
  return 0;
}

static void free_channel_buffer(ChannelBuffer *buffer) {
  free(buffer->data);
  buffer->data = NULL;
  buffer->len = 0;
  buffer->capacity = 0;
}

static int append_channel_data(int fd, ChannelBuffer *buffer) {
  char chunk[4096];

  for (;;) {
    ssize_t count = read(fd, chunk, sizeof(chunk));
    if (count < 0 && (errno == EINTR || errno == EAGAIN ||
                      errno == EWOULDBLOCK)) {
      return 0;
    }
    if (count < 0 || count == 0) {
      return count == 0 ? 1 : -1;
    }

    size_t amount = (size_t)count;
    if (buffer->len > SIZE_MAX - amount) {
      return -1;
    }
    size_t required = buffer->len + amount;
    if (required > buffer->capacity) {
      size_t capacity = buffer->capacity == 0 ? 4096 : buffer->capacity;
      while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
          capacity = required;
          break;
        }
        capacity *= 2;
      }
      char *expanded = realloc(buffer->data, capacity);
      if (expanded == NULL) {
        return -1;
      }
      buffer->data = expanded;
      buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->len, chunk, amount);
    buffer->len += amount;
  }
}

static int take_channel_line(ChannelBuffer *buffer, char **line) {
  char *newline = memchr(buffer->data, '\n', buffer->len);
  if (newline == NULL) {
    return 0;
  }

  size_t length = (size_t)(newline - buffer->data);
  char *result = malloc(length + 1);
  if (result == NULL) {
    return -1;
  }
  memcpy(result, buffer->data, length);
  result[length] = '\0';

  size_t consumed = length + 1;
  memmove(buffer->data, buffer->data + consumed, buffer->len - consumed);
  buffer->len -= consumed;
  *line = result;
  return 1;
}

static void free_pending_requests(void) {
  PendingRequest *request = pending_requests;
  while (request != NULL) {
    PendingRequest *next = request->next;
    free(request->hash);
    free(request->key);
    free(request);
    request = next;
  }
  pending_requests = NULL;
}

static int add_pending_request(const char *hash, const char *key,
                               Child *return_child) {
  PendingRequest *request = malloc(sizeof(*request));
  if (request == NULL) {
    return -1;
  }
  request->hash = NULL;
  request->key = NULL;
  request->return_child = return_child;
  request->next = pending_requests;
  if (copy_range(hash, strlen(hash), &request->hash) != 0 ||
      copy_range(key, strlen(key), &request->key) != 0) {
    free(request->hash);
    free(request->key);
    free(request);
    return -1;
  }
  pending_requests = request;
  return 0;
}

static PendingRequest *find_pending_request(const char *hash) {
  for (PendingRequest *request = pending_requests; request != NULL;
       request = request->next) {
    if (strcmp(request->hash, hash) == 0) {
      return request;
    }
  }
  return NULL;
}

static void remove_pending_request(PendingRequest *request) {
  PendingRequest **link = &pending_requests;
  while (*link != NULL && *link != request) {
    link = &(*link)->next;
  }
  if (*link == request) {
    *link = request->next;
    free(request->hash);
    free(request->key);
    free(request);
  }
}

static int write_line_fd(int fd, const char *line) {
  size_t length = strlen(line);
  return write_all(fd, line, length) == 0 &&
                 write_all(fd, "\n", 1) == 0
             ? 0
             : -1;
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
    close(child->parent_write_fd);
    close(child->parent_read_fd);
    if (kill(child->pid, SIGTERM) != 0 && errno != ESRCH) {
      perror("kill child");
    }
    while (waitpid(child->pid, NULL, 0) < 0 && errno == EINTR) {
    }
    free(child->command_fifo);
    free(child->reply_fifo);
    free_channel_buffer(&child->input);
    free(child);
    child = next;
  }
  children = NULL;
  free_pending_requests();
  free_channel_buffer(&parent_input);
}

static int add_child(pid_t pid, const char *command_fifo,
                     const char *reply_fifo, int parent_write_fd_value,
                     int parent_read_fd_value) {
  Child *child = malloc(sizeof(*child));
  if (child == NULL) {
    return -1;
  }
  child->pid = pid;
  child->command_fifo = NULL;
  child->reply_fifo = NULL;
  child->parent_write_fd = parent_write_fd_value;
  child->parent_read_fd = parent_read_fd_value;
  child->input.data = NULL;
  child->input.len = 0;
  child->input.capacity = 0;
  child->next = children;

  if (copy_range(command_fifo, strlen(command_fifo), &child->command_fifo) !=
          0 ||
      copy_range(reply_fifo, strlen(reply_fifo), &child->reply_fifo) != 0) {
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
  for (Child *existing = children; existing != NULL;
       existing = existing->next) {
    if (set_close_on_exec(existing->parent_write_fd) != 0 ||
        set_close_on_exec(existing->parent_read_fd) != 0) {
      perror("fcntl");
      return -1;
    }
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
  if (set_close_on_exec(to_child[1]) != 0 ||
      set_close_on_exec(from_child[0]) != 0) {
    perror("fcntl");
    close(to_child[0]);
    close(to_child[1]);
    close(from_child[0]);
    close(from_child[1]);
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
    char read_fd[32];
    char write_fd[32];
    (void)snprintf(read_fd, sizeof(read_fd), "%d", to_child[0]);
    (void)snprintf(write_fd, sizeof(write_fd), "%d", from_child[1]);
    close(to_child[1]);
    close(from_child[0]);
    char *const child_argv[] = {
        (char *)program_path, (char *)"--node", (char *)command_fifo,
        (char *)reply_fifo, read_fd, write_fd, NULL};
    execv(program_path, child_argv);
    perror("exec child");
    _exit(127);
  }

  close(to_child[0]);
  close(from_child[1]);
  if (set_close_on_exec(to_child[1]) != 0 ||
      set_close_on_exec(from_child[0]) != 0 ||
      set_nonblocking(to_child[1]) != 0 ||
      set_nonblocking(from_child[0]) != 0) {
    perror("fcntl");
    close(to_child[1]);
    close(from_child[0]);
    if (kill(pid, SIGTERM) != 0 && errno != ESRCH) {
      perror("kill child");
    }
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
    }
    return -1;
  }
  if (add_child(pid, command_fifo, reply_fifo, to_child[1],
                from_child[0]) != 0) {
    if (kill(pid, SIGTERM) != 0 && errno != ESRCH) {
      perror("kill child");
    }
    close(to_child[1]);
    close(from_child[0]);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
    }
    return -1;
  }
  return 0;
}

static int send_lookup_request(int fd, const char *hash, const char *key) {
  size_t length = 7 + strlen(hash) + 1 + strlen(key);
  char *request = malloc(length + 1);
  if (request == NULL) {
    return -1;
  }
  (void)snprintf(request, length + 1, "LOOKUP %s %s", hash, key);
  int result = write_line_fd(fd, request);
  free(request);
  return result;
}

static int send_value_response(int fd, const char *hash,
                               const char *value) {
  size_t length = 6 + strlen(hash) + 1 + strlen(value);
  char *response = malloc(length + 1);
  if (response == NULL) {
    return -1;
  }
  (void)snprintf(response, length + 1, "VALUE %s %s", hash, value);
  int result = write_line_fd(fd, response);
  free(response);
  return result;
}

static int handle_protocol_line(Child *source_child, const char *line) {
  if (strncmp(line, "LOOKUP ", 7) == 0) {
    const char *hash_start = line + 7;
    const char *separator = strchr(hash_start, ' ');
    char *hash = NULL;
    char *key = NULL;
    if (separator == NULL || separator == hash_start ||
        separator[1] == '\0' ||
        copy_range(hash_start, (size_t)(separator - hash_start), &hash) !=
            0 ||
        copy_range(separator + 1, strlen(separator + 1), &key) != 0 ||
        strchr(key, ' ') != NULL) {
      free(hash);
      free(key);
      return -1;
    }

    Entry *entry = find_entry(key);
    int result;
    if (entry != NULL) {
      result = source_child == NULL
                   ? send_value_response(parent_write_fd, hash, entry->value)
                   : send_value_response(source_child->parent_write_fd, hash,
                                         entry->value);
    } else if (child_mode) {
      result = add_pending_request(hash, key, source_child);
      if (result == 0) {
        result = send_lookup_request(parent_write_fd, hash, key);
        if (result != 0) {
          PendingRequest *request = find_pending_request(hash);
          remove_pending_request(request);
        }
      }
    } else {
      result = -1;
    }
    free(hash);
    free(key);
    return result;
  }

  if (strncmp(line, "VALUE ", 6) == 0) {
    const char *hash_start = line + 6;
    const char *separator = strchr(hash_start, ' ');
    if (separator == NULL || separator == hash_start) {
      return -1;
    }
    char *hash = NULL;
    char *value = NULL;
    if (copy_range(hash_start, (size_t)(separator - hash_start), &hash) !=
            0 ||
        copy_range(separator + 1, strlen(separator + 1), &value) != 0) {
      free(hash);
      free(value);
      return -1;
    }
    PendingRequest *request = find_pending_request(hash);
    if (request == NULL) {
      free(hash);
      free(value);
      return -1;
    }
    int result;
    if (request->return_child != NULL) {
      result = send_value_response(request->return_child->parent_write_fd,
                                   hash, value);
    } else {
      size_t length = strlen(hash) + 1 + strlen(value);
      char *reply = malloc(length + 1);
      if (reply != NULL) {
        (void)snprintf(reply, length + 1, "%s %s", hash, value);
        result = write_line_fd(child_mode ? child_reply_fd : STDOUT_FILENO,
                               reply);
        free(reply);
      } else {
        result = -1;
      }
    }
    remove_pending_request(request);
    free(hash);
    free(value);
    return result;
  }

  return -1;
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
  if (argc == 6 && strcmp(argv[1], "--node") == 0) {
    char *end = NULL;
    long parsed_read_fd = strtol(argv[4], &end, 10);
    int read_fd_valid = *argv[4] != '\0' && *end == '\0' &&
                        parsed_read_fd >= 0 && parsed_read_fd <= INT_MAX;
    long parsed_write_fd = strtol(argv[5], &end, 10);
    int write_fd_valid = *argv[5] != '\0' && *end == '\0' &&
                         parsed_write_fd >= 0 && parsed_write_fd <= INT_MAX;
    if (!read_fd_valid || !write_fd_valid ||
        !valid_fifo_path(argv[2]) || !valid_fifo_path(argv[3])) {
      fprintf(stderr, "Invalid child arguments\n");
      return 1;
    }

    child_mode = 1;
    parent_read_fd = (int)parsed_read_fd;
    parent_write_fd = (int)parsed_write_fd;
    if (set_nonblocking(parent_read_fd) != 0 ||
        set_nonblocking(parent_write_fd) != 0) {
      perror("fcntl");
      return 1;
    }
    if (copy_range(argv[2], strlen(argv[2]), &child_command_fifo) != 0 ||
        copy_range(argv[3], strlen(argv[3]), &child_reply_fifo) != 0) {
      fprintf(stderr, "Unable to copy FIFO path\n");
      free(child_command_fifo);
      free(child_reply_fifo);
      return 1;
    }
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = stop_child;
    if (sigemptyset(&action.sa_mask) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0) {
      perror("sigaction");
      return 1;
    }
    if (mkfifo(child_command_fifo, 0600) != 0 ||
        mkfifo(child_reply_fifo, 0600) != 0) {
      perror("mkfifo");
      unlink(child_command_fifo);
      unlink(child_reply_fifo);
      return 1;
    }
    child_command_fd = open(child_command_fifo, O_RDONLY);
    if (child_command_fd < 0) {
      perror("open command FIFO");
      unlink(child_command_fifo);
      unlink(child_reply_fifo);
      return 1;
    }
    child_reply_fd = open(child_reply_fifo, O_WRONLY);
    if (child_reply_fd < 0) {
      perror("open reply FIFO");
      close(child_command_fd);
      unlink(child_command_fifo);
      unlink(child_reply_fifo);
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
    size_t channel_count = 1 + (child_mode ? 1 : 0);
    for (Child *child = children; child != NULL; child = child->next) {
      channel_count++;
    }
    struct pollfd *poll_fds = calloc(channel_count, sizeof(*poll_fds));
    Child **poll_children = calloc(channel_count, sizeof(*poll_children));
    if (poll_fds == NULL || poll_children == NULL) {
      free(poll_fds);
      free(poll_children);
      exit_status = 1;
      break;
    }

    size_t poll_count = 0;
    poll_fds[poll_count].fd =
        child_mode ? child_command_fd : STDIN_FILENO;
    poll_fds[poll_count].events = POLLIN;
    poll_count++;
    size_t parent_poll_index = SIZE_MAX;
    if (child_mode) {
      parent_poll_index = poll_count;
      poll_fds[poll_count].fd = parent_read_fd;
      poll_fds[poll_count].events = POLLIN;
      poll_count++;
    }
    for (Child *child = children; child != NULL; child = child->next) {
      poll_fds[poll_count].fd = child->parent_read_fd;
      poll_fds[poll_count].events = POLLIN;
      poll_children[poll_count] = child;
      poll_count++;
    }

    int poll_result;
    do {
      poll_result = poll(poll_fds, poll_count, -1);
    } while (poll_result < 0 && errno == EINTR);
    if (poll_result < 0) {
      perror("poll");
      free(poll_fds);
      free(poll_children);
      exit_status = 1;
      break;
    }

    if (child_mode && (poll_fds[parent_poll_index].revents & POLLIN)) {
      int read_result = append_channel_data(parent_read_fd, &parent_input);
      if (read_result < 0) {
        exit_status = 1;
        running = 0;
      } else {
        char *protocol_line = NULL;
        while (take_channel_line(&parent_input, &protocol_line) == 1) {
          if (handle_protocol_line(NULL, protocol_line) != 0) {
            free(protocol_line);
            exit_status = 1;
            running = 0;
            break;
          }
          free(protocol_line);
          protocol_line = NULL;
        }
        free(protocol_line);
      }
    }

    for (size_t index = 0; index < poll_count && running; index++) {
      Child *source_child = poll_children[index];
      if (source_child == NULL ||
          !(poll_fds[index].revents & POLLIN)) {
        continue;
      }
      int read_result =
          append_channel_data(source_child->parent_read_fd,
                              &source_child->input);
      if (read_result < 0) {
        exit_status = 1;
        running = 0;
        break;
      }
      char *protocol_line = NULL;
      while (take_channel_line(&source_child->input, &protocol_line) == 1) {
        if (handle_protocol_line(source_child, protocol_line) != 0) {
          free(protocol_line);
          exit_status = 1;
          running = 0;
          break;
        }
        free(protocol_line);
        protocol_line = NULL;
      }
      free(protocol_line);
    }

    free(poll_fds);
    free(poll_children);
    if (!running) {
      break;
    }

    if (poll_fds[0].revents & (POLLIN | POLLHUP)) {
      char *command_buffer = NULL;
      size_t command_length = 0;
      int read_result = read_command_line(
          child_mode ? child_command_fd : STDIN_FILENO, &input,
          &command_buffer, &command_length);

      if (read_result == READ_LINE_EOF) {
        free(command_buffer);
        break;
      }
      if (read_result == READ_LINE_ERROR) {
        free(command_buffer);
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

  if (parse_two_keys(buf, len, "create", &command_fifo, &reply_fifo) != 0 ||
      !valid_fifo_path(command_fifo) || !valid_fifo_path(reply_fifo)) {
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
  char *node = NULL;
  char *key = NULL;
  char *value = NULL;

  if (parse_node_key_value(buf, len, "store", &node, &key, &value) != 0 &&
      parse_key_value(buf, len, "store", &key, &value) != 0) {
    fprintf(stderr, "Invalid store command\n");
    free(node);
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

  if (parse_node_key(buf, len, "delete", &node, &key) != 0 &&
      parse_single_key(buf, len, "delete", &key) != 0) {
    fprintf(stderr, "Invalid delete command\n");
    free(node);
    return -1;
  }

  int result = remove_entry(key);
  free(node);
  free(key);
  return result;
}

int fetch_handler(char *buf, size_t len) {
  char *node = NULL;
  char *hash = NULL;
  char *key = NULL;
  int node_form =
      parse_node_key(buf, len, "fetch", &node, &key) == 0;

  if (!node_form && parse_two_keys(buf, len, "fetch", &hash, &key) != 0) {
    fprintf(stderr, "Invalid fetch command\n");
    free(node);
    return -1;
  }
  if (node_form) {
    hash = duplicate_string(node);
    free(node);
    if (hash == NULL) {
      free(key);
      return -1;
    }
  }

  Entry *entry = find_entry(key);
  if (entry == NULL) {
    if (child_mode) {
      int result = add_pending_request(hash, key, NULL);
      if (result == 0) {
        result = send_lookup_request(parent_write_fd, hash, key);
      }
      free(hash);
      free(key);
      return result;
    }
    free(hash);
    free(key);
    return 1;
  }

  if (node_form) {
    int result = write_line_fd(child_mode ? child_reply_fd : STDOUT_FILENO,
                               entry->value);
    free(hash);
    free(key);
    return result;
  }

  size_t reply_length = strlen(hash) + 1 + strlen(entry->value);
  char *reply = malloc(reply_length + 1);
  if (reply == NULL) {
    perror("malloc");
    free(hash);
    free(key);
    return 1;
  }
  (void)snprintf(reply, reply_length + 1, "%s %s", hash, entry->value);
  int output_fd = child_mode ? child_reply_fd : STDOUT_FILENO;
  if (write_line_fd(output_fd, reply) != 0) {
    perror("write fetch reply");
    free(reply);
    free(hash);
    free(key);
    return -1;
  }

  free(reply);
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
