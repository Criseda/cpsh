#include "../include/shell.h"

int handle_builtin_commands(char **args) {
  if (strcmp(args[0], "history") == 0) {
    return cpsh_history(args);
  }
  if (strcmp(args[0], "cd") == 0) {
    return cpsh_cd(args);
  }
  if (strcmp(args[0], "exit") == 0) {
    return cpsh_exit(args);
  }
  return 0;  // Indicate that no built-in command was executed
}

int cpsh_execute(char **args) {
  pid_t pid;
  int status;

  if (args[0] == NULL) {
    return 1;  // An empty command was entered
  }

  if (strcmp(args[0], "!!") == 0) {
    args = get_last_command();
    if (args == NULL || args[0] == NULL) {
      fprintf(stderr, "cpsh: no commands in history\n");
      return 1;
    }
  }

  if (args[0][0] == '!' && isdigit(args[0][1])) {
    int n = atoi(&args[0][1]);
    args = get_command_by_number(n);
    if (args == NULL || args[0] == NULL) {
      fprintf(stderr, "cpsh: no such command in history\n");
      return 1;
    }
  }

  if (handle_builtin_commands(args)) {
    return 1;  // A built-in command was executed
  }

  pid = fork();  // Fork the process

  if (pid == 0) {
    // we are the child process
    if (execvp(args[0], args) == -1) {
      fprintf(stderr, "cpsh: command not found: %s \n", args[0]);
    }
    exit(EXIT_FAILURE);
  } else if (pid < 0) {
    fprintf(stderr, "cpsh: error forking\n");
  } else {
    // we are the parent process
    waitpid(pid, &status, WUNTRACED);
  }

  return 1;
}