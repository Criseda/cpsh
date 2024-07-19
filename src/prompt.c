#include "../include/common.h"

char *get_cwd() {
  char cwd[PATH_MAX];
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

char *get_hostname() {
  char hostname[256];
  if (gethostname(hostname, 256) != 0) {
    perror("cpsh");
    return strdup("unknown");
  }
  return strdup(hostname);
}

char *get_username() {
  char *username = getenv("USER");
  if (username == NULL) {
    return strdup("unknown");
  }
  return strdup(username);
}

void cpsh_print_prompt() {
  char *cwd = get_cwd();
  char *hostname = get_hostname();
  char *username = get_username();

  printf("\n%s\n%s@%s> ", cwd, username, hostname);
  free(cwd);
  free(hostname);
  free(username);
  fflush(stdout);
}
