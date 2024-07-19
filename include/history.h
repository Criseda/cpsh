#ifndef HISTORY_H
#define HISTORY_H

#define HISTORY_FILE "/.cpsh_history"
#define HISTORY_SIZE 1000
#define MAX_COMMAND_LENGTH 1024

typedef struct HistoryNode {
  char command[MAX_COMMAND_LENGTH];
  struct HistoryNode *next;
} HistoryNode;

extern HistoryNode *history_head;
extern int history_count;

void ensure_history_file_exists();
HistoryNode *load_history();
void add_to_history(const char *command);
void list_history(int line_number);
void search_history(HistoryNode *history_head, const char *keyword);
void save_history(HistoryNode *history_head);
void free_history(HistoryNode *history_head);
char *get_last_command();
char *get_command_by_number(int line_number);

#endif