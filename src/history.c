#include "../include/shell.h"

#define HISTORY_FILE "/.cpsh_history"
#define HISTORY_SIZE 1000
#define MAX_COMMAND_LENGTH 1024

// .cpsh_history should be in the home folder:  ~/.cpsh_history

// history_head is a global variable that points to the head of the history
// linked list
HistoryNode *history_head = NULL;
int history_count = 0;

char *get_last_command() {
  if (history_head == NULL) {
    return NULL;
  }
  HistoryNode *current = history_head;
  while (current->next != NULL) {
    current = current->next;
  }
  return current->command;
}

char *get_command_by_number(int line_number) {
  if (history_head == NULL) {
    return NULL;
  }
  int current_line = 1;
  HistoryNode *current = history_head;
  while (current != NULL) {
    if (current_line == line_number) {
      return strdup(current->command);
    }
    current = current->next;
    current_line++;
  }
  return NULL;
}

void list_history(int line_number) {
  int current_line = 1;
  for (HistoryNode *current = history_head; current != NULL;
       current = current->next) {
    if (line_number == 0 || line_number == current_line) {
      printf("%d %s\n", current_line, current->command);
      // If a specific line number was requested and found, stop the loop
      if (line_number == current_line) {
        return;
      }
    }
    current_line++;
  }
  // If a specific line number was requested but not found, print an error
  // message
  if (line_number != 0 && current_line <= line_number) {
    fprintf(stderr, "History line %d does not exist.\n", line_number);
  }
}

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

// Load the history from the history file
HistoryNode *load_history() {
  ensure_history_file_exists();

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

  // Attempt to open the file
  FILE *file = fopen(historyFilePath, "r");
  if (file == NULL) {
    perror("Failed to open history file");
    exit(EXIT_FAILURE);
  }

  // Check if the history is already loaded
  if (history_head != NULL) {
    fprintf(stderr, "History is already loaded.\n");
    exit(EXIT_FAILURE);
  }

  // Read each line from the file and add it to the history
  char command[MAX_COMMAND_LENGTH];
  while (fgets(command, sizeof(command), file) != NULL) {
    // Remove the newline character from the end of the command
    size_t length = strlen(command);
    if (command[length - 1] == '\n') {
      command[length - 1] = '\0';
    }

    // Add the command to the history
    add_to_history(command);
  }

  fclose(file);
  return history_head;
}

void remove_oldest_command() {
  if (history_head == NULL) return;
  HistoryNode *temp = history_head;
  history_head = history_head->next;
  free(temp);
  history_count--;
}

// Add a command to the history
void add_to_history(const char *command) {
  // If the history is at max capacity, remove the oldest command
  if (history_count >= HISTORY_SIZE) {
    remove_oldest_command();
  }

  // Allocate memory for the new history node
  HistoryNode *node = malloc(sizeof(HistoryNode));
  if (node == NULL) {
    perror("Failed to allocate memory for history node");
    exit(EXIT_FAILURE);
  }
  strncpy(node->command, command, sizeof(node->command) - 1);
  node->command[sizeof(node->command) - 1] = '\0';  // Ensure null-termination
  node->next = NULL;

  // If the history is empty, directly add the new command
  if (history_head == NULL) {
    history_head = node;
  } else {
    // Find the last node in the history
    HistoryNode *current = history_head;
    while (current->next != NULL) {
      current = current->next;
    }

    // Check if the last command is the same as the command to be added
    if (strcmp(current->command, command) == 0) {
      free(node);  // Do not add if it's a duplicate of the last command
      return;
    }

    // Check if the command is !n, !!, sudo !!, or sudo !n and skip adding if it
    // is
    if (strcmp(command, "!!") == 0 ||
        (command[0] == '!' && isdigit(command[1])) ||
        strcmp(command, "sudo !!") == 0 ||
        (strncmp(command, "sudo !", 6) == 0 && isdigit(command[6]))) {
      free(node);  // Do not add these commands to the history
      return;
    }

    // Add the new node to the end of the history
    current->next = node;
  }
  history_count++;  // Increment the history count
}

// Search the history by keyword
void search_history(HistoryNode *history_head, const char *keyword) {
  int index = 1;
  HistoryNode *current = history_head->next;
  while (current != NULL) {
    if (strstr(current->command, keyword) != NULL) {
      printf("%d: %s\n", index, current->command);
    }
    current = current->next;
    index++;
  }
}

// Save the history to the history file
void save_history(HistoryNode *history_head) {
  ensure_history_file_exists();

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

  // Attempt to open the file
  FILE *file = fopen(historyFilePath, "w");
  if (file == NULL) {
    perror("Failed to open history file");
    exit(EXIT_FAILURE);
  }

  // Write each command to the file
  HistoryNode *current = history_head;
  while (current != NULL) {
    fprintf(file, "%s\n", current->command);
    current = current->next;
  }

  fclose(file);
}

// Free the history linked list
void free_history(HistoryNode *history_head) {
  HistoryNode *current = history_head;
  while (current != NULL) {
    HistoryNode *next = current->next;
    free(current);
    current = next;
  }
}