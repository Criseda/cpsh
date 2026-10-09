#ifndef TRAP_H
#define TRAP_H

#include <signal.h>

extern volatile sig_atomic_t pending_traps; /* a trapped signal arrived */
extern volatile sig_atomic_t got_sigint;    /* SIGINT seen (interactive) */

void signals_init(int interactive);
void dotrap(void);          /* run actions of pending trapped signals */
void run_exit_trap(void);   /* EXIT trap, at most once */
int trap_exit_set(void);    /* an EXIT trap is pending */
void trap_reset_subshell(void);
void trap_ignore_bg(void);  /* async lists ignore SIGINT/SIGQUIT */
/* Signals a child process must reset to SIG_DFL before exec. */
const sigset_t *trap_child_defaults(void);
int trap_builtin(int argc, char **argv);
int signal_number(const char *name); /* -1 if unknown; 0 for EXIT */
const char *signal_name(int sig);
void kill_list(void);

#endif
