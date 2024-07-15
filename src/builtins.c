#include "../include/shell.h"

int cpsh_cd(char **args) {
  if (args[1] == NULL) {
    // No argument provided, change directory to home directory
    const char *home_dir = getenv("HOME");
    if (home_dir == NULL) {
      fprintf(stderr, "cpsh: unable to get home directory\n");
    } else {
      if (chdir(home_dir) != 0) {
        perror("cpsh");
      }
    }
  } else {
    // Check if the argument starts with a tilde (~)
    if (args[1][0] == '~') {
      // Get the HOME environment variable
      const char *home_dir = getenv("HOME");
      if (home_dir == NULL) {
        fprintf(stderr, "cpsh: unable to get home directory\n");
      } else {
        // Allocate memory for the new path
        // +1 for the null terminator, -1 because we're replacing the tilde
        char *new_path = malloc(strlen(home_dir) + strlen(args[1]) - 1 + 1);
        if (new_path == NULL) {
          fprintf(stderr, "cpsh: memory allocation error\n");
          return 1;
        }
        // Construct the new path
        strcpy(new_path, home_dir);
        strcat(new_path, args[1] + 1);  // Skip the tilde

        // Change directory to the new path
        if (chdir(new_path) != 0) {
          perror("cpsh");
          free(new_path);  // in case of error, free the allocated memory
          return 1;
        }
        // Free the allocated memory
        free(new_path);
      }
    } else {
      // Argument provided, change directory to specified path
      if (chdir(args[1]) != 0) {
        perror("cpsh");
      }
    }
  }
  return 1;
}

int cpsh_exit(char **args) {
  return 0;  // Return 0 to signal that the shell should exit
}