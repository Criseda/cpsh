#ifndef JOBS_H
#define JOBS_H

#include "parser.h"

/* Processes and jobs (jobs.c).
 *
 * Every child the shell waits for belongs to a job. A foreground job lives
 * only while the shell waits for it; it enters the job table, and gets a job
 * number, if it is started with `&` or stops. With job control on (set -m
 * and a terminal), each job runs in its own process group and the
 * foreground one owns the terminal. */

struct job;

extern int jobctl; /* job control is active in this shell */

/* forkshell() modes */
enum {
  FORK_FG,   /* part of a foreground job */
  FORK_BG,   /* an asynchronous list */
  FORK_NOJOB /* command substitution: no process group of its own */
};

/* Turn job control on or off to match `set -m`; only the top-level shell
 * does job control, and only with a terminal. */
void setjobctl(int on);

/* A new foreground job for running tree n (used for its text if the job
 * stops). */
struct job *job_new(struct node *n);
/* A new background job, entered in the table. */
struct job *job_new_bg(struct node *n);
/* Fork a process of job jp (NULL for FORK_NOJOB). Returns 0 in the child.
 * Under job control a foreground job is forked rather than spawned, so
 * that its first process can take the terminal before it runs. */
pid_t forkshell(struct job *jp, int mode);
/* Report a background job's number and last process ID (interactive
 * shells with job control). */
void job_announce(struct job *jp);
/* Wait for a foreground job; its status, with pipefail applied. A job that
 * stops is kept in the table; otherwise jp is freed. */
int waitforjob(struct job *jp);

int waitforpid(pid_t pid);
int wait_status(int st);
void jobs_reap(void);   /* collect status changes without blocking */
void jobs_notify(void); /* report them (interactive shells, before a prompt) */
/* Interactive shells: refuse the first attempt to exit with stopped jobs. */
int jobs_stopped_warning(void);
void jobs_new_command(void); /* top-level loop: a command is about to run */

/* `set -b`: report jobs as they change while the line editor waits for
 * input. jobs_async_pending() writes the reports and returns how many
 * lines it wrote, so the editor can redraw. */
void jobs_async_begin(void);
int jobs_async_pending(void);
void jobs_async_end(void);

int jobs_builtin(int argc, char **argv);
int fg_builtin(int argc, char **argv);
int bg_builtin(int argc, char **argv);
int wait_builtin(int argc, char **argv);
/* kill: signal the job named by a %job operand. 0, or -1 after an error. */
int jobs_kill(const char *spec, int sig);

char *cmdsub_run(const char *cmd, size_t *len);

#endif
