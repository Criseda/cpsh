#include "exec.h"
#include "expand.h"
#include "shell.h"

/* Saved descriptors, restored by redir_pop(). Frames are delimited by
 * indexes in `frames`. */
struct savedfd {
  int fd;
  int saved; /* -1: fd was closed before the redirection */
};

static struct savedfd *saves;
static int nsaves, capsaves;
static int *frames;
static int nframes, capframes;

void redir_push(void) {
  if (nframes == capframes) {
    capframes = capframes ? capframes * 2 : 16;
    frames = xrealloc(frames, (size_t)capframes * sizeof(int));
  }
  frames[nframes++] = nsaves;
}

static void save_fd(int fd) {
  if (nframes == 0) return;
  for (int i = frames[nframes - 1]; i < nsaves; i++)
    if (saves[i].fd == fd) return;
  int saved = fcntl(fd, F_DUPFD_CLOEXEC, 10);
  if (saved < 0 && errno != EBADF) sh_error("%d: %s", fd, strerror(errno));
  if (nsaves == capsaves) {
    capsaves = capsaves ? capsaves * 2 : 16;
    saves = xrealloc(saves, (size_t)capsaves * sizeof(*saves));
  }
  saves[nsaves].fd = fd;
  saves[nsaves].saved = saved;
  nsaves++;
}

void redir_pop(void) {
  if (nframes == 0) return;
  int base = frames[--nframes];
  if (nsaves > base) fflush(stdout);
  while (nsaves > base) {
    struct savedfd *s = &saves[--nsaves];
    if (s->saved >= 0) {
      dup2(s->saved, s->fd);
      close(s->saved);
    } else {
      close(s->fd);
    }
  }
}

void redir_commit(void) {
  if (nframes == 0) return;
  int base = frames[nframes - 1];
  while (nsaves > base) {
    struct savedfd *s = &saves[--nsaves];
    if (s->saved >= 0) close(s->saved);
  }
}

void redir_reset(void) {
  while (nframes > 0) redir_pop();
}

/* A pipe whose read end yields the here-document text. */
static int heredoc_fd(struct redir *r) {
  const char *body = r->hd_quoted ? r->heredoc : expand_heredoc(r->heredoc);
  size_t len = strlen(body);
  int p[2];
  if (pipe(p) < 0) return -1;
  if (len <= PIPE_BUF) {
    xwrite(p[1], body, len);
  } else {
    /* too big to buffer: a grandchild writes it so nobody waits on it */
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
      close(p[0]);
      if (fork() == 0) {
        signal(SIGPIPE, SIG_DFL);
        xwrite(p[1], body, len);
      }
      _exit(0);
    }
    if (pid > 0) waitpid(pid, NULL, 0);
  }
  close(p[1]);
  return p[0];
}

static int redirect1(struct redir *r, int save) {
  int fd = r->fd, newfd;
  const char *fname = NULL;
  if (r->type == R_HEREDOC) {
    if (save) save_fd(fd);
    newfd = heredoc_fd(r);
    if (newfd < 0) {
      sh_warn("here-document: %s", strerror(errno));
      return -1;
    }
  } else {
    fname = expand_str(r->word, 0);
    if (r->type == R_DUPIN || r->type == R_DUPOUT) {
      if (strcmp(fname, "-") == 0) {
        if (save) save_fd(fd);
        close(fd);
        return 0;
      }
      if (!is_number(fname)) {
        sh_warn("%s: bad file descriptor number", fname);
        return -1;
      }
      int src = atoi(fname);
      if (fcntl(src, F_GETFD) < 0) {
        sh_warn("%s: %s", fname, strerror(errno));
        return -1;
      }
      if (src == fd) return 0;
      if (save) save_fd(fd);
      if (dup2(src, fd) < 0) {
        sh_warn("%d: %s", fd, strerror(errno));
        return -1;
      }
      return 0;
    }
    if (save) save_fd(fd);
    switch (r->type) {
      case R_IN:
        newfd = open(fname, O_RDONLY);
        break;
      case R_OUT:
        if (Cflag) {
          newfd = open(fname, O_WRONLY | O_CREAT | O_EXCL, 0666);
          if (newfd < 0 && errno == EEXIST) {
            struct stat st;
            if (stat(fname, &st) == 0 && !S_ISREG(st.st_mode)) {
              newfd = open(fname, O_WRONLY);
            } else {
              sh_warn("%s: cannot overwrite existing file", fname);
              return -1;
            }
          }
          break;
        }
        /* fall through */
      case R_CLOBBER:
        newfd = open(fname, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        break;
      case R_APPEND:
        newfd = open(fname, O_WRONLY | O_CREAT | O_APPEND, 0666);
        break;
      default: /* R_RDWR */
        newfd = open(fname, O_RDWR | O_CREAT, 0666);
        break;
    }
    if (newfd < 0) {
      sh_warn("%s: %s", fname, strerror(errno));
      return -1;
    }
  }
  if (newfd != fd) {
    if (dup2(newfd, fd) < 0) {
      int e = errno;
      close(newfd);
      sh_warn("%d: %s", fd, strerror(e));
      return -1;
    }
    close(newfd);
  }
  return 0;
}

int apply_redirs(struct redir *r, int save) {
  if (!r) return 0;
  fflush(stdout);
  for (; r; r = r->next)
    if (redirect1(r, save) < 0) return -1;
  return 0;
}
