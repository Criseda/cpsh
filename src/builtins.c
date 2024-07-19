#include "../include/shell.h"

// TODO: history {int n} should truncate the list to the last  n commands
int cpsh_history(char **args) {
  if (args[1] == NULL) {
    list_history(0);
  } else {
    if (strcmp(args[1], "-c") == 0) {  // clear history
      free_history(history_head);
      history_head = NULL;
      history_count = 0;
    } else if (strcmp(args[1], "-w") == 0) {  // write history to file
      save_history(history_head);
    } else if (strcmp(args[1], "-r") == 0) {  // read history from file
      free_history(history_head);
      history_head = load_history();
    } else if (strcmp(args[1], "-a") == 0) {  // add command to history
      if (args[2] == NULL) {
        fprintf(stderr, "cpsh: missing argument for -a\n");
      } else {
        int i = 2;
        char *command = malloc(strlen(args[i]) + 1);
        strcpy(command, args[i]);
        i++;
        while (args[i] != NULL) {
          // concatenate all the arguments into command
          command = realloc(command, strlen(command) + strlen(args[i]) + 2);
          strcat(command, " ");
          strcat(command, args[i]);
          i++;
        }
        add_to_history(command);
        free(command);
      }
    } else if (strcmp(args[1], "-n") == 0) {  // list n-th command
      if (args[2] == NULL) {
        fprintf(stderr, "cpsh: missing argument for -n\n");
      } else {
        int line_number = atoi(args[2]);
        list_history(line_number);
      }
    } else if (strcmp(args[1], "-s") == 0) {  // search history (by keyword)
      if (args[2] == NULL) {
        fprintf(stderr, "cpsh: missing argument for -s\n");
      } else {
        search_history(history_head, args[2]);
      }
    } else {
      fprintf(stderr, "cpsh: unknown option: %s\n", args[1]);
    }
  }
  return 1;
}

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
  exit(0);  // Return 0 to signal that the shell should exit
}