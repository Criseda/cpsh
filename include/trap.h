#ifndef TRAP_H
#define TRAP_H

#include <signal.h>

extern volatile sig_atomic_t pending_traps; /* a trapped signal arrived */
extern volatile sig_atomic_t got_sigint;    /* SIGINT seen (interactive) */
extern volatile sig_atomic_t last_trapped_sig; /* most recent trapped signal */
extern volatile sig_atomic_t got_sigchld;   /* SIGCHLD seen (trap_sigchld) */

void signals_init(int interactive);
void dotrap(void);          /* run actions of pending trapped signals */
void run_exit_trap(void);   /* EXIT trap, at most once */
int trap_exit_set(void);    /* an EXIT trap is pending */
int traps_set(void);        /* any trap with an action is set */
void trap_reset_subshell(void);
void trap_ignore_bg(void);  /* async lists ignore SIGINT/SIGQUIT */
/* Job control: the shell ignores SIGTSTP, SIGTTIN and SIGTTOU (children
 * get the defaults back) unless a trap is set for them. */
void trap_jobctl(int on);
/* Ignore them for good: in a child left in the shell's process group,
 * which a stop signal must not stop since nothing could continue it. */
void trap_ignore_jobctl(void);
void trap_sigchld(int on);  /* catch SIGCHLD, setting got_sigchld */
/* Act on a SIGINT the shell did not receive itself: run its trap, or
 * unwind to the top level. */
void trap_interrupt(void);
/* Signals a child process must reset to SIG_DFL before exec. */
const sigset_t *trap_child_defaults(void);
int trap_builtin(int argc, char **argv);
int signal_number(const char *name); /* -1 if unknown; 0 for EXIT */
const char *signal_name(int sig);
void kill_list(void);

#endif
