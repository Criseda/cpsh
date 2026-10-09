#ifndef EXEC_H
#define EXEC_H

#include "parser.h"

/* evaltree() flags */
#define EV_EXIT 1   /* exit with the status when done (forked child) */
#define EV_TESTED 2 /* status is tested: errexit does not apply */

int evaltree(struct node *n, int flags);
/* Parse and run every command in a string (eval, -c, command substitution). */
int evalstring(const char *s, int flags);
/* Parse and run commands from a source until EOF (scripts, dot, the
 * interactive loop). */
int evalsource(struct source *src, int toplevel);
extern arena *toplevel_arena; /* tree being run by the top-level loop */
extern int cur_lineno;        /* line of the command being run ($LINENO) */

/* break / continue / return */
enum { SKIP_NONE, SKIP_BREAK, SKIP_CONT, SKIP_RETURN };
extern int evalskip, skipcount, loopnest, funcnest, dotnest;

/* Restore the shell to a clean state after an error unwound the stack. */
void reset_after_error(void);

/* ---- command lookup (exec.c) ---- */
void hash_clear(void);
char *path_lookup(const char *name, const char *path); /* malloc'd or NULL */
const char *hash_get(const char *name);
void hash_print(void);
NORETURN void shellexec(const char *path, char **argv, char **envp);

/* ---- functions (exec.c) ---- */
struct func {
  struct func *next;
  char *name;
  struct node *body;
  arena *a;
};
struct func *func_lookup(const char *name);
int func_unset(const char *name);

/* ---- redirections (redir.c) ---- */
int apply_redirs(struct redir *r, int save);
void redir_push(void);
void redir_pop(void);
void redir_commit(void); /* make the current frame's redirections permanent */
void redir_reset(void);

/* ---- processes (jobs.c) ---- */
pid_t forkshell(int background);
int waitforpid(pid_t pid);
int wait_status(int st);
void jobs_add(pid_t pid);
void jobs_reap(void);
int jobs_wait(pid_t pid, int *found); /* blocking; pid 0 = all */
char *cmdsub_run(const char *cmd, size_t *len);

#endif
