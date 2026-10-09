#ifndef HISTORY_H
#define HISTORY_H

#define HISTORY_FILE "/.cpsh_history"
#define HISTORY_SIZE 1000

void history_init(void); /* load the history file */
void history_add(const char *line);
void history_save(void);
void history_clear(void);
int history_first(void); /* number of the oldest entry */
int history_last(void);  /* number of the newest entry, 0 if empty */
const char *history_get(int n);
/* Expand !! !n !-n !prefix in an interactive line. Returns a malloc'd
 * string, or NULL (after printing an error) if an event is not found.
 * *changed is set when any expansion happened. */
char *history_expand(const char *line, int *changed);
int history_builtin(int argc, char **argv);

#endif
