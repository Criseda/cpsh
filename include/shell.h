#ifndef SHELL_H
#define SHELL_H

#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern volatile sig_atomic_t print_prompt;

// singal_handlers.c
void sigint_handler(int sig);
void setup_sigint_handler();

// builtins.c
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

#endif