#ifndef SP_H
#define SP_H

#include <stdio.h>

typedef enum {
  CREATE,
  STORE,
  DELETE,
  FETCH,
  KILL,
  UPDATE,
  UNKNOWN
} CommandType;

typedef struct {
  char *buf;
  size_t len;
  CommandType type;
} Command;

CommandType classify_command(const char *buf, size_t len);
int command_handler(Command *cmd);
int create_handler(char *buf, size_t len);
int store_handler(char *buf, size_t len);
int delete_handler(char *buf, size_t len);
int fetch_handler(char *buf, size_t len);
int kill_handler(char *buf, size_t len);
int update_handler(char *buf, size_t len);

#endif // SP_H