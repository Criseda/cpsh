#include "trap.h"

#include "exec.h"
#include "shell.h"

#define NSIGS CPSH_NSIG

volatile sig_atomic_t pending_traps;
volatile sig_atomic_t got_sigint;
volatile sig_atomic_t last_trapped_sig;

static volatile sig_atomic_t sig_pending[NSIGS];
static char *traps[NSIGS]; /* NULL: default; "": ignore; else action */
static char ignored_on_entry[NSIGS];
static char shell_changed[NSIGS]; /* disposition the shell itself altered */
static sigset_t child_defaults;
static int in_trap;
static int exit_trap_done;

static const struct {
  int sig;
  const char *name;
} signames[] = {
    {SIGHUP, "HUP"},   {SIGINT, "INT"},   {SIGQUIT, "QUIT"}, {SIGILL, "ILL"},
    {SIGTRAP, "TRAP"}, {SIGABRT, "ABRT"}, {SIGBUS, "BUS"},   {SIGFPE, "FPE"},
    {SIGKILL, "KILL"}, {SIGUSR1, "USR1"}, {SIGSEGV, "SEGV"}, {SIGUSR2, "USR2"},
    {SIGPIPE, "PIPE"}, {SIGALRM, "ALRM"}, {SIGTERM, "TERM"}, {SIGCHLD, "CHLD"},
    {SIGCONT, "CONT"}, {SIGSTOP, "STOP"}, {SIGTSTP, "TSTP"}, {SIGTTIN, "TTIN"},
    {SIGTTOU, "TTOU"}, {SIGURG, "URG"},   {SIGXCPU, "XCPU"}, {SIGXFSZ, "XFSZ"},
    {SIGVTALRM, "VTALRM"}, {SIGPROF, "PROF"}, {SIGSYS, "SYS"},
};

#define NSIGNAMES (sizeof(signames) / sizeof(signames[0]))

const char *signal_name(int sig) {
  if (sig == 0) return "EXIT";
  for (size_t i = 0; i < NSIGNAMES; i++)
    if (signames[i].sig == sig) return signames[i].name;
  return NULL;
}

int signal_number(const char *name) {
  if (is_number(name)) {
    int n = atoi(name);
    return n < NSIGS ? n : -1;
  }
  if (strcmp(name, "EXIT") == 0) return 0;
  if (strncmp(name, "SIG", 3) == 0) name += 3;
  for (size_t i = 0; i < NSIGNAMES; i++)
    if (strcmp(signames[i].name, name) == 0) return signames[i].sig;
  return -1;
}

void kill_list(void) {
  for (size_t i = 0; i < NSIGNAMES; i++) puts(signames[i].name);
}

static void on_signal(int sig) {
  if (sig == SIGINT) got_sigint = 1;
  if (sig > 0 && sig < NSIGS && traps[sig]) {
    sig_pending[sig] = 1;
    last_trapped_sig = sig;
    pending_traps = 1;
  }
}

static void set_disposition(int sig, void (*h)(int)) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = h;
  sigemptyset(&sa.sa_mask);
  /* no SA_RESTART: blocking reads must return so traps run promptly */
  sigaction(sig, &sa, NULL);
}

static void update_child_defaults(void) {
  sigemptyset(&child_defaults);
  for (int s = 1; s < NSIGS; s++)
    if (shell_changed[s] && !(traps[s] && !*traps[s]))
      sigaddset(&child_defaults, s);
}

void signals_init(int interactive) {
  for (int s = 1; s < NSIGS; s++) {
    struct sigaction old;
    if (sigaction(s, NULL, &old) == 0 && old.sa_handler == SIG_IGN)
      ignored_on_entry[s] = 1;
  }
  if (interactive) {
    set_disposition(SIGINT, on_signal);
    shell_changed[SIGINT] = 1;
    set_disposition(SIGQUIT, SIG_IGN);
    shell_changed[SIGQUIT] = 1;
    set_disposition(SIGTERM, SIG_IGN);
    shell_changed[SIGTERM] = 1;
  }
  update_child_defaults();
}

const sigset_t *trap_child_defaults(void) { return &child_defaults; }

void dotrap(void) {
  if (!pending_traps || in_trap) return;
  pending_traps = 0;
  int saved = exitstatus;
  in_trap = 1;
  for (int s = 1; s < NSIGS; s++) {
    if (!sig_pending[s]) continue;
    sig_pending[s] = 0;
    if (traps[s] && *traps[s]) {
      char *action = xstrdup(traps[s]);
      evalstring(action, 0);
      free(action);
      exitstatus = saved;
    }
  }
  in_trap = 0;
}

int trap_exit_set(void) { return traps[0] != NULL && !exit_trap_done; }

void run_exit_trap(void) {
  if (exit_trap_done || !traps[0]) return;
  exit_trap_done = 1;
  char *action = traps[0];
  traps[0] = NULL;
  int saved = exitstatus;
  evalstring(action, 0);
  exitstatus = saved;
  free(action);
}

void trap_reset_subshell(void) {
  for (int s = 0; s < NSIGS; s++) {
    if (traps[s] && *traps[s]) {
      free(traps[s]);
      traps[s] = NULL;
      if (s) set_disposition(s, SIG_DFL);
    }
  }
  /* The interactive shell's own handlers do not apply to subshells. */
  for (int s = 1; s < NSIGS; s++)
    if (shell_changed[s] && !traps[s]) {
      set_disposition(s, SIG_DFL);
      shell_changed[s] = 0;
    }
  update_child_defaults();
  exit_trap_done = 0;
}

void trap_ignore_bg(void) {
  set_disposition(SIGINT, SIG_IGN);
  set_disposition(SIGQUIT, SIG_IGN);
  shell_changed[SIGINT] = shell_changed[SIGQUIT] = 0;
  update_child_defaults();
}

static void print_traps(void) {
  strbuf sb;
  sb_init(&sb);
  for (int s = 0; s < NSIGS; s++) {
    if (!traps[s]) continue;
    const char *nm = signal_name(s);
    sb.len = 0;
    sb_puts(&sb, "trap -- ");
    sh_quote(&sb, traps[s]);
    sb_putc(&sb, ' ');
    if (nm) {
      sb_puts(&sb, nm);
    } else {
      char buf[16];
      snprintf(buf, sizeof(buf), "%d", s);
      sb_puts(&sb, buf);
    }
    puts(sb.s);
  }
  sb_free(&sb);
}

int trap_builtin(int argc, char **argv) {
  int i = 1;
  if (i < argc && strcmp(argv[i], "--") == 0) i++;
  if (i >= argc || (strcmp(argv[i], "-p") == 0 && i + 1 == argc)) {
    print_traps();
    return 0;
  }
  const char *action = argv[i];
  /* "trap N..." with a numeric first operand resets those signals */
  int reset = strcmp(action, "-") == 0;
  if (!reset && is_number(action)) {
    reset = 1;
  } else {
    i++;
  }
  int status = 0;
  for (; i < argc; i++) {
    int s = signal_number(argv[i]);
    if (s < 0 || s == SIGKILL || s == SIGSTOP) {
      /* POSIX: not an error that aborts the shell, just a failure */
      sh_warn("trap: %s: bad trap", argv[i]);
      status = 1;
      continue;
    }
    if (s > 0 && ignored_on_entry[s] && !iflag) continue;
    free(traps[s]);
    traps[s] = reset ? NULL : xstrdup(action);
    if (s == 0) continue;
    if (reset) {
      /* back to what the shell would do without a trap */
      if (iflag && rootshell && (s == SIGINT)) {
        set_disposition(s, on_signal);
      } else if (iflag && rootshell && (s == SIGQUIT || s == SIGTERM)) {
        set_disposition(s, SIG_IGN);
      } else {
        set_disposition(s, SIG_DFL);
        shell_changed[s] = 0;
      }
    } else {
      set_disposition(s, *action ? on_signal : SIG_IGN);
      shell_changed[s] = 1;
    }
  }
  update_child_defaults();
  return status;
}
