#define _GNU_SOURCE
#include "sp.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
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
/* Dynamic Output Buffer for non-blocking writes                             */
/* ========================================================================= */

typedef struct {
  char *data;
  size_t len;
  size_t cap;
  size_t head;
} OutputBuffer;

static void outbuf_init(OutputBuffer *ob) {
  ob->data = NULL;
  ob->len = 0;
  ob->cap = 0;
  ob->head = 0;
}

static void outbuf_free(OutputBuffer *ob) {
  free(ob->data);
  ob->data = NULL;
  ob->len = 0;
  ob->cap = 0;
  ob->head = 0;
}

static int outbuf_has_pending(const OutputBuffer *ob) {
  return ob->len > ob->head;
}

static int outbuf_append(OutputBuffer *ob, const char *data, size_t len) {
  if (len == 0) {
    return 0;
  }

  if (ob->head == ob->len) {
    ob->head = 0;
    ob->len = 0;
  } else if (ob->head > 0 && ob->head >= ob->len / 2) {
    memmove(ob->data, ob->data + ob->head, ob->len - ob->head);
    ob->len -= ob->head;
    ob->head = 0;
  }

  if (ob->len + len > ob->cap) {
    size_t new_cap = ob->cap ? ob->cap * 2 : 1024;
    while (new_cap < ob->len + len) {
      if (new_cap > SIZE_MAX / 2) {
        new_cap = ob->len + len;
        break;
      }
      new_cap *= 2;
    }
    char *p = realloc(ob->data, new_cap);
    if (p == NULL) {
      return -1;
    }
    ob->data = p;
    ob->cap = new_cap;
  }

  memcpy(ob->data + ob->len, data, len);
  ob->len += len;
  return 0;
}

static int outbuf_flush(OutputBuffer *ob, int fd) {
  while (ob->len > ob->head) {
    ssize_t n = write(fd, ob->data + ob->head, ob->len - ob->head);
    if (n > 0) {
      ob->head += (size_t)n;
    } else if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0;
      }
      return -1;
    } else {
      return 0;
    }
  }
  ob->head = 0;
  ob->len = 0;
  return 0;
}

static int send_to_stream(int fd, OutputBuffer *ob, const char *data, size_t len) {
  if (fd < 0 || len == 0) {
    return 0;
  }
  if (!outbuf_has_pending(ob)) {
    ssize_t n = write(fd, data, len);
    if (n >= 0 && (size_t)n == len) {
      return 0;
    }
    size_t written = (n > 0) ? (size_t)n : 0;
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      return -1;
    }
    return outbuf_append(ob, data + written, len - written);
  } else {
    return outbuf_append(ob, data, len);
  }
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
/* Child Process Representation and Pending Request Tracking                 */
/* ========================================================================= */

typedef struct ChildNode {
  pid_t pid;
  int from_child_fd;
  int to_child_fd;
  LineBuffer in_buf;
  OutputBuffer out_buf;
  struct ChildNode *next;
} ChildNode;

typedef enum {
  CLIENT_JUDGE,
  CLIENT_CHILD
} ClientType;

typedef struct PendingReq {
  char *hash;
  ClientType client_type;
  int child_to_fd;
  struct PendingReq *next;
} PendingReq;

typedef struct {
  int is_root;
  int is_terminating;

  int cmd_fd;
  int reply_fd;
  LineBuffer cmd_in_buf;
  OutputBuffer reply_out_buf;

  int from_parent_fd;
  int to_parent_fd;
  LineBuffer parent_in_buf;
  OutputBuffer parent_out_buf;

  ChildNode *children;
  PendingReq *pending_reqs;

  char *cmd_fifo_path;
  char *reply_fifo_path;

  char self_path[PATH_MAX];
} NodeState;

static NodeState g_node;

static void add_pending_req(const char *hash, ClientType type, int child_to_fd) {
  PendingReq *req = malloc(sizeof(*req));
  if (req == NULL) {
    return;
  }
  req->hash = duplicate_string(hash);
  req->client_type = type;
  req->child_to_fd = child_to_fd;
  req->next = g_node.pending_reqs;
  g_node.pending_reqs = req;
}

static PendingReq *find_and_remove_pending_req(const char *hash) {
  PendingReq *prev = NULL;
  PendingReq *curr = g_node.pending_reqs;
  while (curr != NULL) {
    if (strcmp(curr->hash, hash) == 0) {
      if (prev == NULL) {
        g_node.pending_reqs = curr->next;
      } else {
        prev->next = curr->next;
      }
      curr->next = NULL;
      return curr;
    }
    prev = curr;
    curr = curr->next;
  }
  return NULL;
}

static ChildNode *find_child_by_to_fd(int to_child_fd) {
  for (ChildNode *c = g_node.children; c != NULL; c = c->next) {
    if (c->to_child_fd == to_child_fd) {
      return c;
    }
  }
  return NULL;
}

static void remove_child_node(ChildNode *target) {
  ChildNode *prev = NULL;
  ChildNode *curr = g_node.children;
  while (curr != NULL) {
    if (curr == target) {
      if (prev == NULL) {
        g_node.children = curr->next;
      } else {
        prev->next = curr->next;
      }
      lb_free(&curr->in_buf);
      outbuf_free(&curr->out_buf);
      free(curr);
      return;
    }
    prev = curr;
    curr = curr->next;
  }
}

/* ========================================================================= */
/* FD Utilities                                                              */
/* ========================================================================= */

static int set_cloexec(int fd) {
  int flags = fcntl(fd, F_GETFD, 0);
  if (flags < 0) {
    return -1;
  }
  return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static int set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return -1;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void get_self_exe_path(char *path, size_t size, const char *argv0) {
  ssize_t len = readlink("/proc/self/exe", path, size - 1);
  if (len > 0) {
    path[len] = '\0';
  } else {
    strncpy(path, argv0, size - 1);
    path[size - 1] = '\0';
  }
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
/* Handlers for Commands and Protocol Messages                               */
/* ========================================================================= */

static int create_handler(const char *buf, size_t len) {
  char *cmd_fifo = NULL;
  char *reply_fifo = NULL;

  if (parse_two_keys(buf, len, "create", &cmd_fifo, &reply_fifo) != 0) {
    fprintf(stderr, "Invalid create command\n");
    return -1;
  }

  int p_to_c[2];
  int c_to_p[2];

  if (pipe(p_to_c) != 0 || pipe(c_to_p) != 0) {
    perror("pipe");
    free(cmd_fifo);
    free(reply_fifo);
    return -1;
  }

  /* Parent ends must have CLOEXEC so further child processes do not inherit them */
  set_cloexec(p_to_c[1]);
  set_cloexec(c_to_p[0]);

  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    close(p_to_c[0]);
    close(p_to_c[1]);
    close(c_to_p[0]);
    close(c_to_p[1]);
    free(cmd_fifo);
    free(reply_fifo);
    return -1;
  }

  if (pid == 0) {
    /* Child process */
    close(p_to_c[1]);
    close(c_to_p[0]);

    char in_str[16];
    char out_str[16];
    snprintf(in_str, sizeof(in_str), "%d", p_to_c[0]);
    snprintf(out_str, sizeof(out_str), "%d", c_to_p[1]);

    execl(g_node.self_path, "sp", "--child", cmd_fifo, reply_fifo, in_str, out_str, (char *)NULL);
    perror("execl");
    _exit(127);
  }

  /* Parent process */
  close(p_to_c[0]);
  close(c_to_p[1]);

  set_nonblocking(c_to_p[0]);
  set_nonblocking(p_to_c[1]);

  ChildNode *child = malloc(sizeof(*child));
  if (child == NULL) {
    perror("malloc child");
    close(c_to_p[0]);
    close(p_to_c[1]);
    free(cmd_fifo);
    free(reply_fifo);
    return -1;
  }

  child->pid = pid;
  child->from_child_fd = c_to_p[0];
  child->to_child_fd = p_to_c[1];
  lb_init(&child->in_buf);
  outbuf_init(&child->out_buf);
  child->next = g_node.children;
  g_node.children = child;

  free(cmd_fifo);
  free(reply_fifo);
  return 0;
}

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
  if (entry != NULL) {
    /* Found in local table */
    size_t hlen = strlen(hash);
    size_t vlen = strlen(entry->value);
    size_t msg_len = hlen + 1 + vlen + 1;
    char *msg = malloc(msg_len + 1);
    if (msg != NULL) {
      snprintf(msg, msg_len + 1, "%s %s\n", hash, entry->value);
      send_to_stream(g_node.reply_fd, &g_node.reply_out_buf, msg, msg_len);
      free(msg);
    }
    free(hash);
    free(key);
    return 0;
  }

  /* Not found in local table */
  if (!g_node.is_root) {
    /* Ask parent via upward query */
    add_pending_req(hash, CLIENT_JUDGE, -1);
    size_t hlen = strlen(hash);
    size_t klen = strlen(key);
    size_t msg_len = 2 + hlen + 1 + klen + 1;
    char *msg = malloc(msg_len + 1);
    if (msg != NULL) {
      snprintf(msg, msg_len + 1, "Q %s %s\n", hash, key);
      send_to_stream(g_node.to_parent_fd, &g_node.parent_out_buf, msg, msg_len);
      free(msg);
    }
    free(hash);
    free(key);
    return 0;
  } else {
    /* Miss at SP main: Consultant will be hooked in Stage D */
    free(hash);
    free(key);
    return 0;
  }
}

static void trigger_termination(void) {
  if (g_node.is_terminating) {
    return;
  }
  g_node.is_terminating = 1;

  /* Tell all children to terminate by closing write pipes */
  for (ChildNode *c = g_node.children; c != NULL; c = c->next) {
    if (c->to_child_fd >= 0) {
      close(c->to_child_fd);
      c->to_child_fd = -1;
    }
  }
}

static int dispatch_command(const char *line, size_t len) {
  CommandType type = classify_command(line, len);
  switch (type) {
  case CREATE:
    return create_handler(line, len);
  case STORE:
    return store_handler(line, len);
  case DELETE:
    return delete_handler(line, len);
  case FETCH:
    return fetch_handler(line, len);
  case KILL:
    trigger_termination();
    return 0;
  case UPDATE:
    /* Consultant update in Stage D */
    return 0;
  default:
    fprintf(stderr, "Unknown command\n");
    return -1;
  }
}

static void handle_child_msg(ChildNode *child, const char *line, size_t len) {
  if (len >= 2 && line[0] == 'Q' && line[1] == ' ') {
    char *hash = NULL;
    char *key = NULL;
    if (parse_two_keys(line, len, "Q", &hash, &key) == 0) {
      Entry *entry = find_entry(key);
      if (entry != NULL) {
        /* Answer found! Send down to child: "A <hash> <value>\n" */
        size_t hlen = strlen(hash);
        size_t vlen = strlen(entry->value);
        size_t msg_len = 2 + hlen + 1 + vlen + 1;
        char *msg = malloc(msg_len + 1);
        if (msg != NULL) {
          snprintf(msg, msg_len + 1, "A %s %s\n", hash, entry->value);
          send_to_stream(child->to_child_fd, &child->out_buf, msg, msg_len);
          free(msg);
        }
      } else {
        /* Parent also misses */
        if (!g_node.is_root) {
          add_pending_req(hash, CLIENT_CHILD, child->to_child_fd);
          size_t hlen = strlen(hash);
          size_t klen = strlen(key);
          size_t msg_len = 2 + hlen + 1 + klen + 1;
          char *msg = malloc(msg_len + 1);
          if (msg != NULL) {
            snprintf(msg, msg_len + 1, "Q %s %s\n", hash, key);
            send_to_stream(g_node.to_parent_fd, &g_node.parent_out_buf, msg, msg_len);
            free(msg);
          }
        } else {
          /* Consultant in Stage D */
        }
      }
      free(hash);
      free(key);
    }
  }
}

static void handle_parent_msg(const char *line, size_t len) {
  if (len >= 2 && line[0] == 'A' && line[1] == ' ') {
    char *hash = NULL;
    char *val = NULL;
    if (parse_key_value(line, len, "A", &hash, &val) == 0) {
      PendingReq *req = find_and_remove_pending_req(hash);
      if (req != NULL) {
        if (req->client_type == CLIENT_JUDGE) {
          size_t hlen = strlen(hash);
          size_t vlen = strlen(val);
          size_t msg_len = hlen + 1 + vlen + 1;
          char *msg = malloc(msg_len + 1);
          if (msg != NULL) {
            snprintf(msg, msg_len + 1, "%s %s\n", hash, val);
            send_to_stream(g_node.reply_fd, &g_node.reply_out_buf, msg, msg_len);
            free(msg);
          }
        } else if (req->client_type == CLIENT_CHILD) {
          ChildNode *target_child = find_child_by_to_fd(req->child_to_fd);
          if (target_child != NULL) {
            size_t hlen = strlen(hash);
            size_t vlen = strlen(val);
            size_t msg_len = 2 + hlen + 1 + vlen + 1;
            char *msg = malloc(msg_len + 1);
            if (msg != NULL) {
              snprintf(msg, msg_len + 1, "A %s %s\n", hash, val);
              send_to_stream(target_child->to_child_fd, &target_child->out_buf, msg, msg_len);
              free(msg);
            }
          }
        }
        free(req->hash);
        free(req);
      }
      free(hash);
      free(val);
    }
  }
}

/* ========================================================================= */
/* Cleanup on Exit                                                           */
/* ========================================================================= */

static void cleanup_and_exit(int exit_code) {
  if (g_node.reply_fd >= 0 && outbuf_has_pending(&g_node.reply_out_buf)) {
    outbuf_flush(&g_node.reply_out_buf, g_node.reply_fd);
  }

  if (g_node.cmd_fd >= 0) {
    close(g_node.cmd_fd);
  }
  if (g_node.reply_fd >= 0) {
    close(g_node.reply_fd);
  }
  if (g_node.from_parent_fd >= 0) {
    close(g_node.from_parent_fd);
  }
  if (g_node.to_parent_fd >= 0) {
    close(g_node.to_parent_fd);
  }

  if (!g_node.is_root) {
    if (g_node.cmd_fifo_path != NULL) {
      unlink(g_node.cmd_fifo_path);
      free(g_node.cmd_fifo_path);
      g_node.cmd_fifo_path = NULL;
    }
    if (g_node.reply_fifo_path != NULL) {
      unlink(g_node.reply_fifo_path);
      free(g_node.reply_fifo_path);
      g_node.reply_fifo_path = NULL;
    }
  }

  lb_free(&g_node.cmd_in_buf);
  outbuf_free(&g_node.reply_out_buf);
  lb_free(&g_node.parent_in_buf);
  outbuf_free(&g_node.parent_out_buf);

  while (g_node.pending_reqs != NULL) {
    PendingReq *next = g_node.pending_reqs->next;
    free(g_node.pending_reqs->hash);
    free(g_node.pending_reqs);
    g_node.pending_reqs = next;
  }

  free_table();
  exit(exit_code);
}

/* ========================================================================= */
/* Main Process Entry and Event Loop                                         */
/* ========================================================================= */

int main(int argc, char *argv[]) {
  signal(SIGPIPE, SIG_IGN);
  memset(&g_node, 0, sizeof(g_node));

  get_self_exe_path(g_node.self_path, sizeof(g_node.self_path), argv[0]);

  if (argc >= 6 && strcmp(argv[1], "--child") == 0) {
    /* Child Mode */
    g_node.is_root = 0;
    g_node.cmd_fifo_path = duplicate_string(argv[2]);
    g_node.reply_fifo_path = duplicate_string(argv[3]);
    g_node.from_parent_fd = atoi(argv[4]);
    g_node.to_parent_fd = atoi(argv[5]);

    set_cloexec(g_node.from_parent_fd);
    set_cloexec(g_node.to_parent_fd);
    set_nonblocking(g_node.from_parent_fd);
    set_nonblocking(g_node.to_parent_fd);

    if (mkfifo(g_node.cmd_fifo_path, 0600) != 0 && errno != EEXIST) {
      perror("mkfifo cmd");
    }
    if (mkfifo(g_node.reply_fifo_path, 0600) != 0 && errno != EEXIST) {
      perror("mkfifo reply");
    }

    /* Open reply FIFO for writing first to rendezvous with judge's reader thread */
    g_node.reply_fd = open(g_node.reply_fifo_path, O_WRONLY | O_CLOEXEC);
    if (g_node.reply_fd < 0) {
      perror("open reply fifo");
      cleanup_and_exit(1);
    }
    g_node.cmd_fd = open(g_node.cmd_fifo_path, O_RDONLY | O_CLOEXEC);
    if (g_node.cmd_fd < 0) {
      perror("open cmd fifo");
      cleanup_and_exit(1);
    }

    set_nonblocking(g_node.reply_fd);
    set_nonblocking(g_node.cmd_fd);
  } else {
    /* Root (SP main) Mode */
    g_node.is_root = 1;
    g_node.cmd_fd = STDIN_FILENO;
    g_node.reply_fd = STDOUT_FILENO;
    g_node.from_parent_fd = -1;
    g_node.to_parent_fd = -1;

    set_nonblocking(STDIN_FILENO);
    set_nonblocking(STDOUT_FILENO);
  }

  lb_init(&g_node.cmd_in_buf);
  outbuf_init(&g_node.reply_out_buf);
  lb_init(&g_node.parent_in_buf);
  outbuf_init(&g_node.parent_out_buf);

  for (;;) {
    if (g_node.is_terminating && g_node.children == NULL) {
      cleanup_and_exit(0);
    }

    /* Count number of descriptors to poll */
    int max_pfds = 4;
    for (ChildNode *c = g_node.children; c != NULL; c = c->next) {
      max_pfds += 2;
    }

    struct pollfd *pfds = malloc(sizeof(struct pollfd) * max_pfds);
    if (pfds == NULL) {
      perror("malloc pfds");
      cleanup_and_exit(1);
    }

    int npfds = 0;

    /* 1. Command channel */
    int cmd_pfd_idx = -1;
    if (g_node.cmd_fd >= 0 && !g_node.is_terminating) {
      cmd_pfd_idx = npfds;
      pfds[npfds].fd = g_node.cmd_fd;
      pfds[npfds].events = POLLIN;
      pfds[npfds].revents = 0;
      npfds++;
    }

    /* 2. Reply channel */
    int reply_pfd_idx = -1;
    if (g_node.reply_fd >= 0 && outbuf_has_pending(&g_node.reply_out_buf)) {
      reply_pfd_idx = npfds;
      pfds[npfds].fd = g_node.reply_fd;
      pfds[npfds].events = POLLOUT;
      pfds[npfds].revents = 0;
      npfds++;
    }

    /* 3. Parent channel */
    int parent_in_pfd_idx = -1;
    if (g_node.from_parent_fd >= 0) {
      parent_in_pfd_idx = npfds;
      pfds[npfds].fd = g_node.from_parent_fd;
      pfds[npfds].events = POLLIN;
      pfds[npfds].revents = 0;
      npfds++;
    }

    int parent_out_pfd_idx = -1;
    if (g_node.to_parent_fd >= 0 && outbuf_has_pending(&g_node.parent_out_buf)) {
      parent_out_pfd_idx = npfds;
      pfds[npfds].fd = g_node.to_parent_fd;
      pfds[npfds].events = POLLOUT;
      pfds[npfds].revents = 0;
      npfds++;
    }

    /* 4. Children channels */
    for (ChildNode *c = g_node.children; c != NULL; c = c->next) {
      if (c->from_child_fd >= 0) {
        pfds[npfds].fd = c->from_child_fd;
        pfds[npfds].events = POLLIN;
        pfds[npfds].revents = 0;
        npfds++;
      }
      if (c->to_child_fd >= 0 && outbuf_has_pending(&c->out_buf)) {
        pfds[npfds].fd = c->to_child_fd;
        pfds[npfds].events = POLLOUT;
        pfds[npfds].revents = 0;
        npfds++;
      }
    }

    int ret = poll(pfds, npfds, -1);
    if (ret < 0) {
      free(pfds);
      if (errno == EINTR) {
        continue;
      }
      perror("poll");
      cleanup_and_exit(1);
    }

    /* Handle write readiness first */
    if (reply_pfd_idx >= 0 && (pfds[reply_pfd_idx].revents & POLLOUT)) {
      outbuf_flush(&g_node.reply_out_buf, g_node.reply_fd);
    }
    if (parent_out_pfd_idx >= 0 && (pfds[parent_out_pfd_idx].revents & POLLOUT)) {
      outbuf_flush(&g_node.parent_out_buf, g_node.to_parent_fd);
    }

    /* Handle read readiness */
    if (cmd_pfd_idx >= 0 && (pfds[cmd_pfd_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
      int eof = 0;
      lb_read_from_fd(&g_node.cmd_in_buf, g_node.cmd_fd, &eof);

      char *line = NULL;
      size_t line_len = 0;
      while (lb_pop_line(&g_node.cmd_in_buf, &line, &line_len) == 1) {
        dispatch_command(line, line_len);
        free(line);
      }

      if (eof) {
        if (g_node.is_root) {
          trigger_termination();
        }
      }
    }

    if (parent_in_pfd_idx >= 0 && (pfds[parent_in_pfd_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
      int eof = 0;
      lb_read_from_fd(&g_node.parent_in_buf, g_node.from_parent_fd, &eof);

      char *line = NULL;
      size_t line_len = 0;
      while (lb_pop_line(&g_node.parent_in_buf, &line, &line_len) == 1) {
        handle_parent_msg(line, line_len);
        free(line);
      }

      if (eof) {
        /* Parent closed pipe -> tell child to exit */
        trigger_termination();
      }
    }

    /* Check each child */
    ChildNode *curr = g_node.children;
    while (curr != NULL) {
      ChildNode *next = curr->next;
      int child_exited = 0;

      for (int i = 0; i < npfds; i++) {
        if (pfds[i].fd == curr->to_child_fd && (pfds[i].revents & POLLOUT)) {
          outbuf_flush(&curr->out_buf, curr->to_child_fd);
        }
        if (pfds[i].fd == curr->from_child_fd && (pfds[i].revents & (POLLIN | POLLHUP | POLLERR))) {
          int eof = 0;
          lb_read_from_fd(&curr->in_buf, curr->from_child_fd, &eof);

          char *line = NULL;
          size_t line_len = 0;
          while (lb_pop_line(&curr->in_buf, &line, &line_len) == 1) {
            handle_child_msg(curr, line, line_len);
            free(line);
          }

          if (eof) {
            child_exited = 1;
          }
        }
      }

      if (child_exited) {
        close(curr->from_child_fd);
        if (curr->to_child_fd >= 0) {
          close(curr->to_child_fd);
        }
        waitpid(curr->pid, NULL, WNOHANG);
        remove_child_node(curr);
      }

      curr = next;
    }

    free(pfds);
  }

  return 0;
}
