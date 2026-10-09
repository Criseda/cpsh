#include "jobs.h"

#include <termios.h>

#include "exec.h"
#include "expand.h"
#include "shell.h"
#include "trap.h"

enum { PS_RUNNING, PS_STOPPED, PS_DONE };

struct proc {
  pid_t pid;
  int wstat; /* last status from waitpid; -1 if it could not be waited for */
  int state;
};

struct job {
  struct proc *procs;
  int nprocs, cap;
  pid_t pgid;        /* process group, when started under job control */
  int num;           /* job number; 0 while not in the table */
  int changed;       /* state changed since it was last reported */
  int bg;            /* running in the background */
  unsigned long seq; /* recency, for the current and previous jobs */
  struct node *n;    /* tree it runs, until its text is needed */
  char *cmd;         /* command text, once in the table */
  char **ptext;      /* text of each process of a pipeline, for jobs -l */
  int nptext;
  int has_tmodes;
  struct termios tmodes; /* terminal modes it had when it stopped */
};

#define MAXJOBS 1024

int jobctl;
static int ttyfd = -1;
static pid_t initialpgrp; /* terminal's process group before job control */
static struct termios shell_tmodes;
static int have_shell_tmodes;

static struct job **table; /* ordered by job number */
static int njobs, capjobs;
static int inherited; /* table is a subshell's copy of its parent's jobs */
static unsigned long seqno;
static int job_warning; /* commands left before a stopped-jobs warning lapses */
static struct job *forking; /* job a child was forked for, kept reachable */

/* ---- jobs and their processes ---- */

static int job_state(const struct job *jp) {
  int stopped = 0;
  for (int i = 0; i < jp->nprocs; i++) {
    if (jp->procs[i].state == PS_RUNNING) return PS_RUNNING;
    if (jp->procs[i].state == PS_STOPPED) stopped = 1;
  }
  return stopped ? PS_STOPPED : PS_DONE;
}

static void proc_update(struct proc *p, int st) {
  p->wstat = st;
  if (WIFSTOPPED(st))
    p->state = PS_STOPPED;
#ifdef WIFCONTINUED
  else if (WIFCONTINUED(st))
    p->state = PS_RUNNING;
#endif
  else
    p->state = PS_DONE;
}

static int proc_status(const struct proc *p) {
  int st = p->wstat;
  if (st == -1) return 127;
  if (WIFEXITED(st)) return WEXITSTATUS(st);
  if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
  if (WIFSTOPPED(st)) return 128 + WSTOPSIG(st);
  return 1;
}

/* The last stopped process, whose signal describes a stopped job. */
static const struct proc *stopped_proc(const struct job *jp) {
  for (int i = jp->nprocs - 1; i >= 0; i--)
    if (jp->procs[i].state == PS_STOPPED) return &jp->procs[i];
  return NULL;
}

/* Exit status of a finished job: its last process, or with pipefail the
 * last one that failed. */
static int job_status(const struct job *jp) {
  int status = 0, failed = 0;
  for (int i = 0; i < jp->nprocs; i++) {
    status = proc_status(&jp->procs[i]);
    if (status) failed = status;
  }
  return optval[OPT_pipefail] && failed ? failed : status;
}

struct job *job_new(struct node *n) {
  struct job *jp = xcalloc(1, sizeof(*jp));
  jp->n = n;
  return jp;
}

static void job_free(struct job *jp) {
  free(jp->procs);
  free(jp->cmd);
  for (int i = 0; i < jp->nptext; i++) free(jp->ptext[i]);
  free(jp->ptext);
  free(jp);
}

static void job_addproc(struct job *jp, pid_t pid) {
  if (jp->nprocs == jp->cap) {
    jp->cap = jp->cap ? jp->cap * 2 : 4;
    jp->procs = xrealloc(jp->procs, (size_t)jp->cap * sizeof(*jp->procs));
  }
  struct proc *p = &jp->procs[jp->nprocs++];
  p->pid = pid;
  p->wstat = 0;
  p->state = PS_RUNNING;
}

/* A subshell lists its parent's jobs (so `$(jobs -p)` works) until it
 * does anything with jobs of its own; they are not its children. */
static void drop_inherited(void) {
  if (!inherited) return;
  inherited = 0;
  for (int i = 0; i < njobs; i++) job_free(table[i]);
  njobs = 0;
}

static void job_remove(struct job *jp) {
  for (int i = 0; i < njobs; i++) {
    if (table[i] == jp) {
      memmove(table + i, table + i + 1, (size_t)(njobs - i - 1) * sizeof(*table));
      njobs--;
      break;
    }
  }
  jp->num = 0;
}

/* Give jp a job number and keep it in the table. */
static void job_enter(struct job *jp) {
  drop_inherited();
  if (njobs == MAXJOBS) {
    /* forget the oldest finished job, or else the oldest one */
    jobs_reap();
    int i = 0;
    while (i < njobs && job_state(table[i]) != PS_DONE) i++;
    struct job *old = table[i < njobs ? i : 0];
    job_remove(old);
    job_free(old);
  }
  if (njobs == capjobs) {
    capjobs = capjobs ? capjobs * 2 : 16;
    table = xrealloc(table, (size_t)capjobs * sizeof(*table));
  }
  jp->num = njobs ? table[njobs - 1]->num + 1 : 1;
  table[njobs++] = jp;
  if (!jp->cmd) {
    jp->cmd = node_text(jp->n);
    struct node *n = jp->n;
    if (n && n->type == N_PIPE && !n->redirs) {
      jp->nptext = n->u.list.n;
      jp->ptext = xmalloc((size_t)jp->nptext * sizeof(*jp->ptext));
      for (int i = 0; i < jp->nptext; i++)
        jp->ptext[i] = node_text(n->u.list.items[i]);
    }
  }
  jp->n = NULL;
}

struct job *job_new_bg(struct node *n) {
  struct job *jp = job_new(n);
  jp->bg = 1;
  jp->seq = ++seqno;
  job_enter(jp);
  return jp;
}

/* Whether a ranks before b for the current job: stopped jobs first, then
 * the most recently started, stopped or continued. */
static int ranks_before(const struct job *a, const struct job *b) {
  int sa = job_state(a) == PS_STOPPED, sb = job_state(b) == PS_STOPPED;
  return sa != sb ? sa : a->seq > b->seq;
}

/* The current job (k = 0) or the previous one (k = 1). */
static struct job *current_job(int k) {
  struct job *best[2] = {NULL, NULL};
  for (int i = 0; i < njobs; i++) {
    struct job *jp = table[i];
    if (!best[0] || ranks_before(jp, best[0])) {
      best[1] = best[0];
      best[0] = jp;
    } else if (!best[1] || ranks_before(jp, best[1])) {
      best[1] = jp;
    }
  }
  return best[k];
}

/* '+' for the current job, '-' for the previous one */
static int job_mark(const struct job *jp) {
  return jp == current_job(0) ? '+' : jp == current_job(1) ? '-' : ' ';
}

static struct job *find_num(int num) {
  for (int i = 0; i < njobs; i++)
    if (table[i]->num == num) return table[i];
  return NULL;
}

static struct job *find_pid(pid_t pid) {
  for (int i = 0; i < njobs; i++)
    for (int j = 0; j < table[i]->nprocs; j++)
      if (table[i]->procs[j].pid == pid) return table[i];
  return NULL;
}

/* Look up a job operand: %%, %+, %, %-, %n, %string, %?string, or a
 * process ID. Reports failures as `who: spec: reason`. */
static struct job *getjob(const char *spec, const char *who) {
  struct job *jp = NULL;
  const char *err = "no such job";
  if (!spec || strcmp(spec, "%") == 0 || strcmp(spec, "%%") == 0 ||
      strcmp(spec, "%+") == 0) {
    jp = current_job(0);
    err = "no current job";
  } else if (strcmp(spec, "%-") == 0) {
    jp = current_job(1);
    err = "no previous job";
  } else if (spec[0] == '%' && is_number(spec + 1)) {
    jp = find_num(atoi(spec + 1));
  } else if (spec[0] == '%') {
    int contains = spec[1] == '?';
    const char *s = spec + 1 + contains;
    size_t len = strlen(s);
    for (int i = 0; i < njobs; i++) {
      const char *cmd = table[i]->cmd;
      if (contains ? strstr(cmd, s) == NULL : strncmp(cmd, s, len) != 0)
        continue;
      if (jp) {
        jp = NULL;
        err = "ambiguous job";
        break;
      }
      jp = table[i];
    }
  } else if (is_number(spec)) {
    jp = find_pid((pid_t)atol(spec));
  }
  if (!jp) sh_warn("%s: %s: %s", who, spec ? spec : "%%", err);
  return jp;
}

/* ---- reporting ---- */

/* Describe a state: wstat is the status that put the job or process in it
 * and status its exit status, once done. */
static void describe(int state, int wstat, int status, char *buf, size_t size) {
  if (state == PS_RUNNING) {
    snprintf(buf, size, "Running");
  } else if (state == PS_STOPPED) {
    int sig = WSTOPSIG(wstat);
    const char *nm = signal_name(sig);
    if (sig == SIGTSTP || !nm)
      snprintf(buf, size, "Stopped");
    else
      snprintf(buf, size, "Stopped (SIG%s)", nm);
  } else if (wstat != -1 && WIFSIGNALED(wstat)) {
    const char *msg = strsignal(WTERMSIG(wstat));
    if (!msg) msg = "Killed";
    /* macOS appends the number ("Terminated: 15") */
    int len = (int)strcspn(msg, ":");
#ifdef WCOREDUMP
    snprintf(buf, size, "%.*s%s", len, msg,
             WCOREDUMP(wstat) ? " (core dumped)" : "");
#else
    snprintf(buf, size, "%.*s", len, msg);
#endif
  } else if (status) {
    snprintf(buf, size, "Done(%d)", status);
  } else {
    snprintf(buf, size, "Done");
  }
}

static void describe_job(const struct job *jp, char *buf, size_t size) {
  int state = job_state(jp);
  int wstat = state == PS_STOPPED ? stopped_proc(jp)->wstat
                                  : jp->procs[jp->nprocs - 1].wstat;
  describe(state, wstat, state == PS_DONE ? job_status(jp) : 0, buf, size);
}

/* jobs -l for a pipeline: a line per process, giving its state where it
 * differs from the line before. */
static void print_procs(FILE *f, const struct job *jp, const char *amp) {
  int pidw = 0;
  for (int i = 0; i < jp->nprocs; i++) {
    char b[24];
    int w = snprintf(b, sizeof(b), "%ld", (long)jp->procs[i].pid);
    if (w > pidw) pidw = w;
  }
  char head[32];
  int headw = snprintf(head, sizeof(head), "[%d]%c", jp->num, job_mark(jp));
  char prev[64] = "";
  for (int i = 0; i < jp->nprocs; i++) {
    const struct proc *p = &jp->procs[i];
    char state[64];
    describe(p->state, p->wstat, proc_status(p), state, sizeof(state));
    const char *shown = i && strcmp(state, prev) == 0 ? "" : state;
    fprintf(f, "%-*s %*ld %-24s%s%s%s\n", headw, i ? "" : head, pidw,
            (long)p->pid, shown, i ? "| " : "", jp->ptext[i],
            i == jp->nprocs - 1 ? amp : "");
    snprintf(prev, sizeof(prev), "%s", state);
  }
}

/* One line of `jobs` output, or of a notification. mode is 0, 'l' or
 * 'p'. */
static void print_job(FILE *f, const struct job *jp, int mode) {
  pid_t leader = jp->pgid ? jp->pgid : jp->procs[0].pid;
  if (mode == 'p') {
    fprintf(f, "%ld\n", (long)leader);
    return;
  }
  const char *amp = job_state(jp) == PS_RUNNING && jp->bg ? " &" : "";
  if (mode == 'l' && jp->nprocs > 1 && jp->nptext == jp->nprocs) {
    print_procs(f, jp, amp);
    return;
  }
  char state[64];
  describe_job(jp, state, sizeof(state));
  int mark = job_mark(jp);
  if (mode == 'l')
    fprintf(f, "[%d]%c %ld %-24s%s%s\n", jp->num, mark, (long)leader, state,
            jp->cmd, amp);
  else
    fprintf(f, "[%d]%c  %-24s%s%s\n", jp->num, mark, state, jp->cmd, amp);
}

void jobs_reap(void) {
  if (inherited) return;
#ifdef WCONTINUED
  int opts = WNOHANG | WUNTRACED | WCONTINUED;
#else
  int opts = WNOHANG | WUNTRACED;
#endif
  for (int i = 0; i < njobs; i++) {
    struct job *jp = table[i];
    int before = job_state(jp);
    for (int j = 0; j < jp->nprocs; j++) {
      struct proc *p = &jp->procs[j];
      if (p->state == PS_DONE) continue;
      int st;
      pid_t r;
      do r = waitpid(p->pid, &st, opts);
      while (r < 0 && errno == EINTR);
      if (r == p->pid) {
        proc_update(p, st);
      } else if (r < 0) {
        p->state = PS_DONE;
        p->wstat = -1;
      }
    }
    if (job_state(jp) != before) jp->changed = 1;
  }
}

/* Report jobs that finished or stopped since the last report, and forget
 * the finished ones. Returns the number of lines written. */
static int notify_changed(void) {
  jobs_reap();
  int lines = 0;
  for (int i = 0; i < njobs;) {
    struct job *jp = table[i];
    int state = job_state(jp);
    if (jp->changed && state != PS_RUNNING) {
      print_job(stderr, jp, 0);
      lines++;
    }
    jp->changed = 0;
    if (state == PS_DONE) {
      job_remove(jp);
      job_free(jp);
      continue;
    }
    i++;
  }
  fflush(stderr);
  return lines;
}

void jobs_notify(void) { notify_changed(); }

int jobs_stopped_warning(void) {
  if (!jobctl || !iflag || job_warning) return 0;
  jobs_reap();
  for (int i = 0; i < njobs; i++) {
    if (job_state(table[i]) == PS_STOPPED) {
      fputs("You have stopped jobs.\n", stderr);
      job_warning = 2;
      return 1;
    }
  }
  return 0;
}

void jobs_new_command(void) {
  if (job_warning) job_warning--;
}

static int async_on;

void jobs_async_begin(void) {
  async_on = jobctl && optval[OPT_b];
  if (async_on) trap_sigchld(1);
}

int jobs_async_pending(void) {
  if (!got_sigchld) return 0;
  got_sigchld = 0;
  return async_on ? notify_changed() : 0;
}

void jobs_async_end(void) {
  if (async_on) trap_sigchld(0);
  async_on = 0;
}

/* ---- the terminal ---- */

static int open_tty(void) {
  int fd = open("/dev/tty", O_RDWR);
  if (fd >= 0) return move_fd_high(fd);
  for (fd = 2; fd >= 0; fd--)
    if (isatty(fd)) return fcntl(fd, F_DUPFD_CLOEXEC, 10);
  return -1;
}

static int sigttin_default(void) {
  struct sigaction sa;
  return sigaction(SIGTTIN, NULL, &sa) == 0 && sa.sa_handler == SIG_DFL;
}

void setjobctl(int on) {
  if (!rootshell || on == jobctl) return;
  if (on) {
    int fd = open_tty();
    pid_t pgrp = -1;
    while (fd >= 0) {
      pgrp = tcgetpgrp(fd);
      if (pgrp < 0 || pgrp == getpgrp()) break;
      /* started in the background: stop until moved to the foreground */
      if (!sigttin_default()) {
        pgrp = -1;
        break;
      }
      kill(0, SIGTTIN);
    }
    if (pgrp < 0) {
      if (fd >= 0) close(fd);
      sh_warn("can't access tty; job control turned off");
      optval[OPT_m] = 0;
      return;
    }
    initialpgrp = pgrp;
    trap_jobctl(1);
    setpgid(0, 0);
    tcsetpgrp(fd, getpid());
    ttyfd = fd;
    have_shell_tmodes = tcgetattr(fd, &shell_tmodes) == 0;
  } else {
    /* hand the terminal back to the process group that had it */
    setpgid(0, initialpgrp);
    tcsetpgrp(ttyfd, initialpgrp);
    trap_jobctl(0);
    close(ttyfd);
    ttyfd = -1;
  }
  jobctl = on;
}

/* After a foreground job: take the terminal back, keeping the job's modes
 * if it stopped and dropping them if a signal killed it. */
static void tty_reclaim(struct job *jp, int state) {
  if (state == PS_STOPPED)
    jp->has_tmodes = tcgetattr(ttyfd, &jp->tmodes) == 0;
  tcsetpgrp(ttyfd, getpgrp());
  const struct proc *last = &jp->procs[jp->nprocs - 1];
  int killed = last->wstat != -1 && WIFSIGNALED(last->wstat);
  if ((state == PS_STOPPED || killed) && have_shell_tmodes)
    tcsetattr(ttyfd, TCSADRAIN, &shell_tmodes);
}

/* ---- processes ---- */

pid_t forkshell(struct job *jp, int mode) {
  int jc = jobctl && mode != FORK_NOJOB;
  pid_t pgid = jc && jp->nprocs ? jp->pgid : 0;
  if (jc && mode == FORK_FG && !jp->nprocs)
    have_shell_tmodes = tcgetattr(ttyfd, &shell_tmodes) == 0;
  fflush(stdout);
  fflush(stderr);
  pid_t pid = fork();
  if (pid < 0) {
    int e = errno;
    if (jp && jp->num) job_remove(jp); /* a background job: no process */
    if (jp) waitforjob(jp);
    sh_error("fork: %s", strerror(e));
  }
  if (pid == 0) {
    rootshell = 0;
    handler = NULL;
    iflag = 0;
    forking = jp;
    if (jc) {
      setpgid(0, pgid);
      /* the shell ignores SIGTTOU until trap_reset_subshell() */
      if (mode == FORK_FG && !pgid) tcsetpgrp(ttyfd, getpid());
    }
    if (ttyfd >= 0) {
      close(ttyfd);
      ttyfd = -1;
    }
    trap_reset_subshell();
    if (jobctl && mode == FORK_NOJOB) trap_ignore_jobctl();
    jobctl = 0;
    inherited = njobs > 0;
    if (mode == FORK_BG && !jc) {
      /* without job control, async lists ignore SIGINT/SIGQUIT and read
       * from /dev/null unless redirected explicitly */
      trap_ignore_bg();
      int fd = open("/dev/null", O_RDONLY);
      if (fd > 0) {
        dup2(fd, 0);
        close(fd);
      }
    }
    return 0;
  }
  if (jp) {
    if (jc) {
      if (!jp->nprocs) jp->pgid = pid;
      setpgid(pid, jp->pgid); /* as the child does: whichever runs first */
    }
    job_addproc(jp, pid);
  }
  return pid;
}

void job_announce(struct job *jp) {
  if (iflag && jobctl)
    fprintf(stderr, "[%d] %ld\n", jp->num, (long)jp->procs[jp->nprocs - 1].pid);
}

int wait_status(int st) {
  if (WIFEXITED(st)) return WEXITSTATUS(st);
  if (WIFSIGNALED(st)) {
    int sig = WTERMSIG(st);
    if (rootshell && iflag && sig != SIGINT && sig != SIGPIPE) {
      const char *msg = strsignal(sig);
#ifdef WCOREDUMP
      fprintf(stderr, "%s%s\n", msg ? msg : "killed",
              WCOREDUMP(st) ? " (core dumped)" : "");
#else
      fprintf(stderr, "%s\n", msg ? msg : "killed");
#endif
    } else if (rootshell && iflag && sig == SIGINT) {
      fputc('\n', stderr);
    }
    return 128 + sig;
  }
  return 1;
}

int waitforpid(pid_t pid) {
  int st;
  for (;;) {
    pid_t r = waitpid(pid, &st, 0);
    if (r == pid) break;
    if (r < 0 && errno != EINTR) return 127;
  }
  return wait_status(st);
}

int waitforjob(struct job *jp) {
  if (!jp->nprocs) { /* abandoned before its first process started */
    job_free(jp);
    return 0;
  }
  int jc = jobctl && jp->pgid;
  for (int i = 0; i < jp->nprocs; i++) {
    struct proc *p = &jp->procs[i];
    while (p->state == PS_RUNNING) {
      int st;
      pid_t r = waitpid(p->pid, &st, jc ? WUNTRACED : 0);
      if (r == p->pid) {
        proc_update(p, st);
      } else if (r < 0 && errno != EINTR) {
        p->state = PS_DONE;
        p->wstat = -1;
      }
    }
  }
  int state = job_state(jp);
  if (jc) tty_reclaim(jp, state);

  if (state == PS_STOPPED) {
    if (!jp->num) job_enter(jp);
    jp->bg = 0;
    jp->changed = 0;
    jp->seq = ++seqno;
    if (iflag) {
      fputc('\n', stderr);
      print_job(stderr, jp, 0);
    }
    return 128 + WSTOPSIG(stopped_proc(jp)->wstat);
  }

  /* report deaths by signal as the shell always has */
  for (int i = 0; i < jp->nprocs; i++)
    if (jp->procs[i].wstat != -1) wait_status(jp->procs[i].wstat);
  int status = job_status(jp);
  const struct proc *last = &jp->procs[jp->nprocs - 1];
  int sigint = last->wstat != -1 && WIFSIGNALED(last->wstat) &&
               WTERMSIG(last->wstat) == SIGINT;
  if (jp->num) job_remove(jp);
  job_free(jp);
  /* under job control ^C reaches only the job: act as if the shell got it
   * too, so that it stops running the rest of the command line */
  if (sigint && jc && iflag) trap_interrupt();
  return status;
}

/* ---- built-ins ---- */

static int need_jobctl(const char *who) {
  if (jobctl) return 1;
  sh_warn("%s: no job control", who);
  return 0;
}

/* A job fg or bg can continue: stopped or running, in a process group. */
static int can_restart(const struct job *jp, const char *who, const char *spec) {
  const char *err = job_state(jp) == PS_DONE ? "job has terminated"
                    : !jp->pgid ? "job not started under job control"
                                : NULL;
  if (err) sh_warn("%s: %s: %s", who, spec ? spec : "%%", err);
  return !err;
}

/* Continue a stopped job in the foreground or background. */
static int restart_job(struct job *jp, int fg) {
  if (fg) {
    have_shell_tmodes = tcgetattr(ttyfd, &shell_tmodes) == 0;
    if (jp->has_tmodes) tcsetattr(ttyfd, TCSADRAIN, &jp->tmodes);
    tcsetpgrp(ttyfd, jp->pgid);
  }
  if (job_state(jp) == PS_STOPPED) kill(-jp->pgid, SIGCONT);
  for (int i = 0; i < jp->nprocs; i++)
    if (jp->procs[i].state == PS_STOPPED) jp->procs[i].state = PS_RUNNING;
  jp->changed = 0;
  jp->bg = !fg;
  jp->seq = ++seqno;
  return fg ? waitforjob(jp) : 0;
}

int fg_builtin(int argc, char **argv) {
  if (!need_jobctl("fg")) return 1;
  int i = 1;
  if (i < argc && strcmp(argv[i], "--") == 0) i++;
  jobs_reap();
  const char *spec = i < argc ? argv[i] : NULL;
  struct job *jp = getjob(spec, "fg");
  if (!jp || !can_restart(jp, "fg", spec)) return 1;
  printf("%s\n", jp->cmd);
  fflush(stdout);
  return restart_job(jp, 1);
}

int bg_builtin(int argc, char **argv) {
  if (!need_jobctl("bg")) return 1;
  int i = 1;
  if (i < argc && strcmp(argv[i], "--") == 0) i++;
  jobs_reap();
  int status = 0;
  do {
    const char *spec = i < argc ? argv[i] : NULL;
    struct job *jp = getjob(spec, "bg");
    if (!jp || !can_restart(jp, "bg", spec)) {
      status = 1;
      continue;
    }
    if (job_state(jp) == PS_RUNNING && jp->bg) continue;
    restart_job(jp, 0);
    printf("[%d]%c %s &\n", jp->num, job_mark(jp), jp->cmd);
  } while (++i < argc);
  fflush(stdout);
  return status;
}

int jobs_builtin(int argc, char **argv) {
  int mode = 0, i = 1;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    for (const char *p = argv[i] + 1; *p; p++) {
      if (*p != 'l' && *p != 'p') {
        sh_warn("jobs: -%c: invalid option", *p);
        return 2;
      }
      mode = *p;
    }
  }
  jobs_reap();
  int status = 0;
  if (i < argc) {
    for (; i < argc; i++) {
      struct job *jp = getjob(argv[i], "jobs");
      if (!jp) {
        status = 1;
        continue;
      }
      print_job(stdout, jp, mode);
      jp->changed = 0;
      if (!inherited && job_state(jp) == PS_DONE) {
        job_remove(jp);
        job_free(jp);
      }
    }
    return status;
  }
  for (int j = 0; j < njobs; j++) {
    print_job(stdout, table[j], mode);
    table[j]->changed = 0;
  }
  /* finished jobs are forgotten once reported */
  for (int j = 0; j < njobs && !inherited;) {
    if (job_state(table[j]) == PS_DONE) {
      struct job *jp = table[j];
      job_remove(jp);
      job_free(jp);
    } else {
      j++;
    }
  }
  return status;
}

/* Wait for a job to finish or stop. A trapped signal interrupts the wait;
 * *interrupted is then set. */
static int wait_job(struct job *jp, int *interrupted) {
  for (int i = 0; i < jp->nprocs; i++) {
    struct proc *p = &jp->procs[i];
    while (p->state == PS_RUNNING) {
      int st;
      pid_t r = waitpid(p->pid, &st, WUNTRACED);
      if (r == p->pid) {
        proc_update(p, st);
      } else if (r < 0 && errno == EINTR) {
        if (pending_traps) {
          *interrupted = 1;
          return 128 + last_trapped_sig;
        }
      } else {
        p->state = PS_DONE;
        p->wstat = -1;
      }
    }
  }
  const struct proc *sp = stopped_proc(jp);
  return sp ? 128 + WSTOPSIG(sp->wstat) : job_status(jp);
}

int wait_builtin(int argc, char **argv) {
  int i = 1, interrupted = 0;
  if (i < argc && strcmp(argv[i], "--") == 0) i++;
  drop_inherited();
  if (i >= argc) {
    /* every known job; stopped ones stay in the table */
    for (int j = 0; j < njobs;) {
      struct job *jp = table[j];
      int status = wait_job(jp, &interrupted);
      if (interrupted) return status;
      if (job_state(jp) == PS_DONE) {
        job_remove(jp);
        job_free(jp);
      } else {
        j++;
      }
    }
    return 0;
  }
  int status = 0;
  for (; i < argc; i++) {
    const char *arg = argv[i];
    struct job *jp;
    pid_t pid = 0;
    if (arg[0] == '%') {
      jp = getjob(arg, "wait");
    } else if (is_number(arg)) {
      pid = (pid_t)atol(arg);
      jp = find_pid(pid);
    } else {
      sh_warn("wait: %s: invalid process id", arg);
      status = 2;
      continue;
    }
    if (!jp) {
      status = 127;
      continue;
    }
    status = wait_job(jp, &interrupted);
    if (interrupted) return status;
    /* the last process of a job ($! after a pipeline) stands for the job */
    if (pid && pid != jp->procs[jp->nprocs - 1].pid) {
      for (int j = 0; j < jp->nprocs; j++)
        if (jp->procs[j].pid == pid && jp->procs[j].state == PS_DONE)
          status = proc_status(&jp->procs[j]);
    }
    if (job_state(jp) == PS_DONE) {
      job_remove(jp);
      job_free(jp);
    }
  }
  return status;
}

int jobs_kill(const char *spec, int sig) {
  struct job *jp = getjob(spec, "kill");
  if (!jp) return -1;
  /* a stopped job acts on SIGTERM or SIGHUP only once continued */
  int cont = (sig == SIGTERM || sig == SIGHUP) && job_state(jp) == PS_STOPPED;
  int r = 0;
  if (jp->pgid) {
    r = kill(-jp->pgid, sig);
    if (r == 0 && cont) kill(-jp->pgid, SIGCONT);
  } else {
    for (int i = 0; i < jp->nprocs; i++) {
      pid_t pid = jp->procs[i].pid;
      if (jp->procs[i].state == PS_DONE) continue;
      if (kill(pid, sig) < 0)
        r = -1;
      else if (cont)
        kill(pid, SIGCONT);
    }
  }
  if (r < 0) {
    sh_warn("kill: %s: %s", spec, strerror(errno));
    return -1;
  }
  return 0;
}

char *cmdsub_run(const char *cmd, size_t *len) {
  int p[2];
  if (pipe(p) < 0) sh_error("pipe: %s", strerror(errno));
  pid_t pid = forkshell(NULL, FORK_NOJOB);
  if (pid == 0) {
    close(p[0]);
    if (p[1] != 1) {
      dup2(p[1], 1);
      close(p[1]);
    }
    evalstring(cmd, EV_EXIT);
  }
  close(p[1]);
  strbuf sb;
  sb_init(&sb);
  char buf[4096];
  for (;;) {
    ssize_t n = read(p[0], buf, sizeof(buf));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    sb_putn(&sb, buf, (size_t)n);
  }
  close(p[0]);
  cmdsub_status = waitforpid(pid);
  *len = sb.len;
  return sb_detach(&sb);
}
