#include "../include/shell.h"

char *get_cwd() {
  char cwd[1024];
  char *home = getenv("HOME");

  if (getcwd(cwd, sizeof(cwd)) == NULL) {
    perror("cpsh");
    return strdup("unknown");
  }

  if (home && strcmp(cwd, home) == 0) {
    return strdup("~");
  }

  if (home && strstr(cwd, home) == cwd) {
    char *relative_path = cwd + strlen(home);
    if (*relative_path == '\0') {
      return strdup("~");
    } else {
      char *result = malloc(strlen(relative_path) + 2);
      sprintf(result, "~%s", relative_path);
      return result;
    }
  }

  return strdup(cwd);
}

void cpsh_print_prompt() {
  char *cwd = get_cwd();
  char hostname[256];
  char *username = getenv("USER");

  if (gethostname(hostname, 256) != 0) {
    perror("cpsh");
    strcpy(hostname, "unknown");
  }

  if (username == NULL) {
    username = "unknown";
  }

  printf("\n%s\n", cwd);
  printf("%s@%s> ", username, hostname);
  free(cwd);
  fflush(stdout);
}

void cpsh_loop(void) {
  char *line;     // line of input
  char **args;    // array of arguments
  int status = 1; // status of the shell

  // Set up the signal handler for SIGINT
  setup_sigint_handler();

  do {
    if (print_prompt) {
      cpsh_print_prompt();
      print_prompt = 0; // Reset the flag after printing
    } else {
      cpsh_print_prompt();
    }
    line = cpsh_read_line();
    args = cpsh_tokenise(line);
    status = cpsh_execute(args);

    free(line); // Free the line variable
    free(args); // Free the args variable
  } while (status);
}
