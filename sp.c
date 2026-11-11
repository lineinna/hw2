#include "sp.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
  if (argc > 1) {
    fprintf(stderr, "Usage: ./sp\n");
    return 1;
  }

  // TODO: Determine whether the current process is SP main.
  //       After that, you may want to make the following code the same for both
  //       SP main and SP child processes. Therefore, you will set some
  //       variables here.

  // end TODO

  while (1) {
    char *command_buffer = NULL;
    Command command = {.buf = command_buffer, .len = 0, .type = UNKNOWN};
    // TODO: poll the STDIO/FIFO for commands.

    // end TODO

    // TODO: poll parent, and child processes for custom commands.

    // end TODO

    // TODO: classify the command and execute it.
    command_handler(&command);
    // end TODO

    // TODO: handle custom commands.

    // end TODO
  }
}

int command_handler(Command *cmd) {
  switch (cmd->type) {
  case CREATE:
    create_handler(cmd->buf, cmd->len);
    break;
  case STORE:
    store_handler(cmd->buf, cmd->len);
    break;
  case DELETE:
    delete_handler(cmd->buf, cmd->len);
    break;
  case FETCH:
    fetch_handler(cmd->buf, cmd->len);
    break;
  case KILL:
    kill_handler(cmd->buf, cmd->len);
    break;
  case UPDATE:
    update_handler(cmd->buf, cmd->len);
    break;
  default:
    fprintf(stderr, "Unknown command type\n");
    return -1;
  }
  return 0;
}

int create_handler(char *buf, size_t len) {
  // TODO: Implement the CREATE command handler.
}

int store_handler(char *buf, size_t len) {
  // TODO: Implement the STORE command handler.
}

int delete_handler(char *buf, size_t len) {
  // TODO: Implement the DELETE command handler.
}

int fetch_handler(char *buf, size_t len) {
  // TODO: Implement the FETCH command handler.
}

int kill_handler(char *buf, size_t len) {
  // TODO: Implement the KILL command handler.
}

int update_handler(char *buf, size_t len) {
  // TODO: Implement the UPDATE command handler.
}
