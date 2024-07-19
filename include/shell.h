#ifndef SHELL_H
#define SHELL_H

#define _POSIX_C_SOURCE 200809L

#define TOKEN_SIZE 64
#define MAX_COMMAND_LENGTH 1024
#define HISTORY_FILE "/.cpsh_history"
#define HISTORY_SIZE 1000

#include <ctype.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern volatile sig_atomic_t print_prompt;

typedef struct HistoryNode {
  char command[MAX_COMMAND_LENGTH];
  struct HistoryNode *next;
} HistoryNode;

extern HistoryNode *history_head;
extern int history_count;

// singal_handlers.c
void sigint_handler(int sig);
void setup_sigint_handler();

// builtins.c
int cpsh_history(char **args);
int cpsh_cd(char **args);
int cpsh_exit(char **args);

// input.c
char *cpsh_read_line();
char **cpsh_tokenise(char *line);

// executor.c
int cpsh_execute(char **args);

// prompt.c
char *get_cwd(void);
void cpsh_printPrompt(void);
void cpsh_loop(void);

// history.c
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