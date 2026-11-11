#ifndef SP_H
#define SP_H

#include <stddef.h>
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

#endif // SP_H
