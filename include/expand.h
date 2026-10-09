#ifndef EXPAND_H
#define EXPAND_H

#include "common.h"

/* Growable argument vector allocated on the scratch stack. */
struct arglist {
  char **v; /* NULL-terminated */
  int n, cap;
};

void arglist_init(struct arglist *al);
void arglist_add(struct arglist *al, char *s);

/* Full expansion of a command word: tilde, parameter, command and arithmetic
 * expansion, field splitting, pathname expansion and quote removal. */
void expand_fields(const char *word, struct arglist *out);
/* Expansion without field splitting or globbing (assignment values,
 * redirection targets, case words). */
char *expand_str(const char *word, int assignment);
/* Expansion producing an fnmatch() pattern: quoted characters are escaped. */
char *expand_pattern(const char *word);
/* Here-document body expansion (parameters, commands, arithmetic). */
char *expand_heredoc(const char *body);
/* Expand a prompt string (parameters, commands, arithmetic). */
char *expand_prompt(const char *s);

/* Exit status of the last command substitution, or -1 if none ran. */
extern int cmdsub_status;

/* arith.c */
long arith_eval(const char *expr);

#endif
