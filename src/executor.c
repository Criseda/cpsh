#include "../include/shell.h"

int cpsh_execute(char **args) {
  pid_t pid;
  int status;

  if (args[0] == NULL) {
    return 1;  // An empty command was entered
  }
  if (strcmp(args[0], "cd") == 0) {
    return cpsh_cd(args);  // Call the cpsh_cd function
  }
  if (strcmp(args[0], "exit") == 0) {
    return cpsh_exit(args);  // Call the cpsh_exit function
  }

  pid = fork();  // Fork the process

  if (pid == 0) {
    // we are the child process
    if (execvp(args[0], args) == -1) {
      printf("cpsh: command not found: %s \n", args[0]);
    }
    exit(EXIT_FAILURE);
  } else if (pid < 0) {
    printf("cpsh: error forking\n");
  } else {
    // we are the parent process
    waitpid(pid, &status, WUNTRACED);
  }

  return 1;
}