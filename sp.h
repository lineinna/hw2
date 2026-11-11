#ifndef SP_H
#define SP_H

#include <stdio.h>

typedef struct {
  char *buf;
  size_t len;
  enum { CREATE, STORE, DELETE, FETCH, KILL, UPDATE, UNKNOWN } type;
} Command;

int command_handler(Command *cmd);
int create_handler(char *buf, size_t len);
int store_handler(char *buf, size_t len);
int delete_handler(char *buf, size_t len);
int fetch_handler(char *buf, size_t len);
int kill_handler(char *buf, size_t len);
int update_handler(char *buf, size_t len);

#endif // SP_H