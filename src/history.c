#include "../include/shell.h"

#define HISTORY_FILE "/.cpsh_history"
#define HISTORY_SIZE 1000

// .cpsh_history should be in the home folder:  ~/.cpsh_history
// if it does not exist there, create it

void ensure_history_file_exists() {
  char historyFilePath[PATH_MAX];

  // Get the path to the home directory
  const char *homeDir = getenv("HOME");
  if (homeDir == NULL) {
    fprintf(stderr, "HOME environment variable is not set.\n");
    exit(EXIT_FAILURE);
  }

  // Construct the path to the history file
  snprintf(historyFilePath, sizeof(historyFilePath), "%s%s", homeDir,
           HISTORY_FILE);

  // Attempt to open or create the file
  FILE *file = fopen(historyFilePath, "a+");
  if (file == NULL) {
    perror("Failed to open history file");
    exit(EXIT_FAILURE);
  }
  fclose(file);
}