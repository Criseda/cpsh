#include "exec.h"
#include "expand.h"
#include "shell.h"
#include "trap.h"

/* Background processes started with `&`, remembered for `wait`. */
struct job {
  pid_t pid;
  int status;
  int done;
};

#define MAXJOBS 1024

static struct job *jobs;
static int njobs, capjobs;

pid_t forkshell(int background) {
  fflush(stdout);
  fflush(stderr);
  pid_t pid = fork();
  if (pid < 0) sh_error("fork: %s", strerror(errno));
  if (pid == 0) {
    rootshell = 0;
    handler = NULL;
    iflag = 0;
    trap_reset_subshell();
    free(jobs);
    jobs = NULL;
    njobs = capjobs = 0;
    if (background) {
      /* without job control, async lists ignore SIGINT/SIGQUIT and read
       * from /dev/null unless redirected explicitly */
      trap_ignore_bg();
      int fd = open("/dev/null", O_RDONLY);
      if (fd > 0) {
        dup2(fd, 0);
        close(fd);
      }
    }
  }
  return pid;
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

void jobs_add(pid_t pid) {
  if (njobs == MAXJOBS) {
    /* forget the oldest finished job */
    int i = 0;
    while (i < njobs && !jobs[i].done) i++;
    if (i == njobs) i = 0;
    memmove(jobs + i, jobs + i + 1, (size_t)(njobs - i - 1) * sizeof(*jobs));
    njobs--;
  }
  if (njobs == capjobs) {
    capjobs = capjobs ? capjobs * 2 : 16;
    jobs = xrealloc(jobs, (size_t)capjobs * sizeof(*jobs));
  }
  jobs[njobs].pid = pid;
  jobs[njobs].status = 0;
  jobs[njobs].done = 0;
  njobs++;
}

static struct job *job_find(pid_t pid) {
  for (int i = 0; i < njobs; i++)
    if (jobs[i].pid == pid) return &jobs[i];
  return NULL;
}

void jobs_reap(void) {
  for (int i = 0; i < njobs; i++) {
    if (jobs[i].done) continue;
    int st;
    pid_t r = waitpid(jobs[i].pid, &st, WNOHANG);
    if (r == jobs[i].pid) {
      jobs[i].done = 1;
      jobs[i].status = WIFEXITED(st) ? WEXITSTATUS(st)
                       : WIFSIGNALED(st) ? 128 + WTERMSIG(st)
                                         : 1;
    }
  }
}

/* Wait for one job (pid > 0) or all of them (pid == 0). A trapped signal
 * interrupts the wait with status 128+n, as POSIX requires. */
int jobs_wait(pid_t pid, int *found) {
  *found = 1;
  int status = 0;
  for (int i = 0; i < njobs; i++) {
    struct job *j = &jobs[i];
    if (pid && j->pid != pid) continue;
    while (!j->done) {
      int st;
      pid_t r = waitpid(j->pid, &st, 0);
      if (r == j->pid) {
        j->done = 1;
        j->status = WIFEXITED(st) ? WEXITSTATUS(st)
                    : WIFSIGNALED(st) ? 128 + WTERMSIG(st)
                                      : 1;
      } else if (r < 0 && errno == EINTR) {
        if (pending_traps) return 128 + SIGINT;
      } else {
        j->done = 1;
        j->status = 127;
      }
    }
    status = j->status;
    if (pid) return status;
  }
  if (pid) {
    *found = job_find(pid) != NULL;
    return 127;
  }
  /* all jobs collected: forget them */
  njobs = 0;
  return status;
}

char *cmdsub_run(const char *cmd, size_t *len) {
  int p[2];
  if (pipe(p) < 0) sh_error("pipe: %s", strerror(errno));
  pid_t pid = forkshell(0);
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
