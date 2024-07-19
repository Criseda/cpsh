#include "../include/shell.h"

#define HISTORY_FILE "/.cpsh_history"
#define HISTORY_SIZE 1000
#define MAX_COMMAND_LENGTH 1024

// .cpsh_history should be in the home folder:  ~/.cpsh_history

// history_head is a global variable that points to the head of the history
// linked list
HistoryNode *history_head = NULL;

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

// Add a command to the history
void add_to_history(const char *command) {
  // If the history is empty, directly add the new command
  if (history_head == NULL) {
    HistoryNode *node = malloc(sizeof(HistoryNode));
    if (node == NULL) {
      perror("Failed to allocate memory for history node");
      exit(EXIT_FAILURE);
    }
    strcpy(node->command, command);
    node->next = NULL;
    history_head = node;
    return;
  }

  // Find the last node in the history
  HistoryNode *current = history_head;
  while (current->next != NULL) {
    current = current->next;
  }

  // Check if the last command is the same as the command to be added
  if (strcmp(current->command, command) == 0) {
    return;  // Do not add if it's a duplicate of the last command
  }

  // Create a new history node for the new command
  HistoryNode *node = malloc(sizeof(HistoryNode));
  if (node == NULL) {
    perror("Failed to allocate memory for history node");
    exit(EXIT_FAILURE);
  }
  strcpy(node->command, command);
  node->next = NULL;

  // Add the new node to the end of the history
  current->next = node;
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