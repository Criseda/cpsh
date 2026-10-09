#include "exec.h"

#include <fnmatch.h>
#include <spawn.h>

#include "builtins.h"
#include "expand.h"
#include "history.h"
#include "shell.h"
#include "trap.h"
#include "vars.h"

int evalskip, skipcount, loopnest, funcnest, dotnest;
arena *toplevel_arena;

extern char **environ;

/* ---- function table ---- */

static struct func *functions;

struct func *func_lookup(const char *name) {
  for (struct func *f = functions; f; f = f->next)
    if (strcmp(f->name, name) == 0) return f;
  return NULL;
}

static void func_define(const char *name, struct node *body, arena *a) {
  struct func *f = func_lookup(name);
  arena_ref(a);
  if (f) {
    arena_unref(f->a);
  } else {
    f = xmalloc(sizeof(*f));
    f->name = xstrdup(name);
    f->next = functions;
    functions = f;
  }
  f->body = body;
  f->a = a;
}

int func_unset(const char *name) {
  for (struct func **pp = &functions; *pp; pp = &(*pp)->next) {
    if (strcmp((*pp)->name, name) == 0) {
      struct func *f = *pp;
      *pp = f->next;
      arena_unref(f->a);
      free(f->name);
      free(f);
      return 0;
    }
  }
  return -1;
}

/* Saved positional parameters of active function calls, so that an error
 * unwinding through them can restore the caller's. */
struct funcframe {
  struct funcframe *prev;
  struct posparams saved;
};
static struct funcframe *frames;

static int callfunction(struct func *f, int argc, char **argv, int flags) {
  struct funcframe fr;
  fr.saved = pos;
  fr.prev = frames;
  frames = &fr;
  pos.argc = 0;
  pos.argv = NULL;
  pos_set(argc - 1, argv + 1);
  arena *a = f->a;
  arena_ref(a); /* the function may redefine itself while running */
  funcnest++;
  var_scope_push();
  int saved_loopnest = loopnest;
  loopnest = 0;
  int status = evaltree(f->body, flags & EV_TESTED);
  loopnest = saved_loopnest;
  var_scope_pop();
  funcnest--;
  arena_unref(a);
  if (evalskip == SKIP_RETURN) {
    evalskip = SKIP_NONE;
    status = exitstatus;
  }
  pos_free(&pos);
  pos = fr.saved;
  frames = fr.prev;
  return status;
}

void reset_after_error(void) {
  while (frames) {
    pos_free(&pos);
    pos = frames->saved;
    frames = frames->prev;
  }
  while (var_scope_depth() > 0) var_scope_pop();
  redir_reset();
  evalskip = skipcount = loopnest = funcnest = dotnest = 0;
  special_soft = 0;
  if (toplevel_arena) {
    arena_unref(toplevel_arena);
    toplevel_arena = NULL;
  }
}

/* ---- command search ---- */

struct hentry {
  struct hentry *next;
  char *name;
  char *path;
};

#define HASHSIZE 64
static struct hentry *hashtab[HASHSIZE];

static unsigned hstr(const char *s) {
  unsigned h = 5381;
  while (*s) h = h * 33 + (unsigned char)*s++;
  return h % HASHSIZE;
}

void hash_clear(void) {
  for (int i = 0; i < HASHSIZE; i++) {
    while (hashtab[i]) {
      struct hentry *e = hashtab[i];
      hashtab[i] = e->next;
      free(e->name);
      free(e->path);
      free(e);
    }
  }
}

static void hash_remove(const char *name) {
  for (struct hentry **pp = &hashtab[hstr(name)]; *pp; pp = &(*pp)->next) {
    if (strcmp((*pp)->name, name) == 0) {
      struct hentry *e = *pp;
      *pp = e->next;
      free(e->name);
      free(e->path);
      free(e);
      return;
    }
  }
}

void hash_print(void) {
  for (int i = 0; i < HASHSIZE; i++)
    for (struct hentry *e = hashtab[i]; e; e = e->next) puts(e->path);
}

char *path_lookup(const char *name, const char *path) {
  if (!path) path = "/usr/bin:/bin";
  size_t nl = strlen(name);
  char *buf = NULL;
  for (const char *p = path;;) {
    const char *colon = strchr(p, ':');
    size_t dl = colon ? (size_t)(colon - p) : strlen(p);
    buf = xrealloc(buf, dl + nl + 3);
    if (dl == 0) {
      buf[0] = '.';
      dl = 1;
    } else {
      memcpy(buf, p, dl);
    }
    buf[dl] = '/';
    memcpy(buf + dl + 1, name, nl + 1);
    struct stat st;
    if (stat(buf, &st) == 0 && S_ISREG(st.st_mode) && access(buf, X_OK) == 0)
      return buf;
    if (!colon) break;
    p = colon + 1;
  }
  free(buf);
  return NULL;
}

const char *hash_get(const char *name) {
  unsigned h = hstr(name);
  for (struct hentry *e = hashtab[h]; e; e = e->next)
    if (strcmp(e->name, name) == 0) return e->path;
  char *path = path_lookup(name, var_get("PATH"));
  if (!path) return NULL;
  struct hentry *e = xmalloc(sizeof(*e));
  e->name = xstrdup(name);
  e->path = path;
  e->next = hashtab[h];
  hashtab[h] = e;
  return path;
}

/* argv for running a script that lacks a #! line with this shell. */
static char **script_argv(const char *path, char **argv) {
  int n = 0;
  while (argv[n]) n++;
  char **nv = stalloc((size_t)(n + 2) * sizeof(char *));
  nv[0] = (char *)shell_exe;
  nv[1] = (char *)path;
  for (int i = 1; i <= n; i++) nv[i + 1] = argv[i];
  return nv;
}

void shellexec(const char *path, char **argv, char **envp) {
  execve(path, argv, envp);
  int e = errno;
  if (e == ENOEXEC) {
    execve(shell_exe, script_argv(path, argv), envp);
    e = errno;
  }
  sh_warn("%s: %s", argv[0], e == ENOENT ? "not found" : strerror(e));
  fflush(stdout);
  _exit(e == ENOENT ? 127 : 126);
}

/* Start an external command without forking the shell: posix_spawn uses
 * vfork-style process creation, so the cost does not grow with the shell's
 * memory. Signal state is only reset when the shell changed some. */
static int spawn(const char *path, char **argv, pid_t *pid) {
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  const sigset_t *defs = trap_child_defaults();
  short flags = 0;
  for (int s = 1; s < CPSH_NSIG; s++)
    if (sigismember(defs, s) == 1) {
      posix_spawnattr_setsigdefault(&attr, defs);
      flags |= POSIX_SPAWN_SETSIGDEF;
      break;
    }
  sigset_t cur;
  sigprocmask(SIG_SETMASK, NULL, &cur);
  for (int s = 1; s < CPSH_NSIG; s++)
    if (sigismember(&cur, s) == 1) {
      sigset_t empty;
      sigemptyset(&empty);
      posix_spawnattr_setsigmask(&attr, &empty);
      flags |= POSIX_SPAWN_SETSIGMASK;
      break;
    }
  posix_spawnattr_setflags(&attr, flags);
  char **env = var_environ();
  int err = posix_spawn(pid, path, NULL, &attr, argv, env);
  if (err == ENOEXEC)
    err = posix_spawn(pid, shell_exe, NULL, &attr, script_argv(path, argv), env);
  posix_spawnattr_destroy(&attr);
  return err;
}

/* ---- tracing ---- */

static void xtrace(char **assigns, int nassigns, char **argv) {
  strbuf sb;
  sb_init(&sb);
  const char *ps4 = var_get("PS4");
  sb_puts(&sb, ps4 ? expand_prompt(ps4) : "+ ");
  int first = 1;
  for (int i = 0; i < nassigns; i++) {
    if (!first) sb_putc(&sb, ' ');
    first = 0;
    const char *eq = strchr(assigns[i], '=');
    sb_putn(&sb, assigns[i], (size_t)(eq - assigns[i] + 1));
    sh_quote(&sb, eq + 1);
  }
  for (; argv && *argv; argv++) {
    if (!first) sb_putc(&sb, ' ');
    first = 0;
    sh_quote(&sb, *argv);
  }
  sb_putc(&sb, '\n');
  fflush(stdout);
  xwrite(2, sb.s, sb.len);
  sb_free(&sb);
}

/* ---- simple commands ---- */

/* Flush a builtin's output; a write error makes it fail. */
static int flush_builtin(const char *name, int status) {
  if (fflush(stdout) != 0 || ferror(stdout)) {
    int e = errno;
    clearerr(stdout);
    sh_warn("%s: write error: %s", name, strerror(e));
    return status ? status : 1;
  }
  return status;
}

enum { K_NOTFOUND, K_SPECIAL, K_FUNC, K_BUILTIN, K_EXTERNAL };

static int assign_all(char **assigns, int n, int flags, int local) {
  for (int i = 0; i < n; i++) {
    if (local) {
      const char *eq = strchr(assigns[i], '=');
      char *name = ststrndup(assigns[i], (size_t)(eq - assigns[i]));
      if (var_local(name) < 0) return -1;
    }
    if (var_set_assign(assigns[i], flags) < 0) return -1;
  }
  return 0;
}

static int evalcommand(struct node *n, int flags) {
  stackmark mark = stmark();
  struct arglist args;
  int status = 0;
  arglist_init(&args);
  cmdsub_status = -1;
  for (int i = 0; i < n->u.simple.argc; i++)
    expand_fields(n->u.simple.argv[i], &args);

  int nassigns = n->u.simple.nassigns;
  char **assigns = NULL;
  if (nassigns) {
    assigns = stalloc((size_t)nassigns * sizeof(char *));
    for (int i = 0; i < nassigns; i++) {
      const char *a = n->u.simple.assigns[i];
      const char *eq = strchr(a, '=');
      const char *val = expand_str(eq + 1, 1);
      size_t nl = (size_t)(eq - a + 1), vl = strlen(val);
      char *s = stalloc(nl + vl + 1);
      memcpy(s, a, nl);
      memcpy(s + nl, val, vl + 1);
      assigns[i] = s;
    }
  }
  if (xflag) xtrace(assigns, nassigns, args.v);

  if (args.n == 0) {
    if (n->redirs) {
      redir_push();
      if (apply_redirs(n->redirs, 1) < 0) status = 1;
      redir_pop();
    }
    if (assign_all(assigns, nassigns, 0, 0) < 0) raise_exception(EX_ERROR);
    if (status == 0 && cmdsub_status >= 0) status = cmdsub_status;
    strelease(mark);
    return status;
  }

  /* `command name`: bypass functions and special-builtin semantics */
  char **argv = args.v;
  int argc = args.n;
  int nofunc = 0;
  const char *search_path = NULL;
  while (argc > 0 && strcmp(argv[0], "command") == 0) {
    int i = 1, p = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
      if (strcmp(argv[i], "--") == 0) {
        i++;
        break;
      }
      if (strcmp(argv[i], "-p") == 0) {
        p = 1;
      } else {
        i = -1; /* -v / -V: let the builtin handle it */
        break;
      }
    }
    if (i < 0 || i >= argc) break;
    if (p) search_path = "/usr/bin:/bin";
    argv += i;
    argc -= i;
    nofunc = 1;
  }

  const char *name = argv[0];
  const struct builtin *bi = NULL;
  struct func *fn = NULL;
  const char *path = NULL;
  int kind;
  if (strchr(name, '/')) {
    kind = K_EXTERNAL;
    path = name;
  } else if ((bi = find_builtin(name)) && bi->special && !nofunc) {
    kind = K_SPECIAL;
  } else if (!nofunc && (fn = func_lookup(name))) {
    kind = K_FUNC;
  } else if (bi) {
    kind = K_BUILTIN;
  } else {
    if (search_path) {
      char *p = path_lookup(name, search_path);
      path = p ? ststrdup(p) : NULL;
      free(p);
    } else {
      path = hash_get(name);
    }
    kind = path ? K_EXTERNAL : K_NOTFOUND;
  }

  switch (kind) {
    case K_SPECIAL:
      redir_push();
      if (apply_redirs(n->redirs, 1) < 0) {
        redir_pop();
        if (!iflag) raise_exception(EX_ERROR);
        status = 1;
        break;
      }
      if (assign_all(assigns, nassigns, bi->fn == exec_builtin ? V_EXPORT : 0,
                     0) < 0)
        raise_exception(EX_ERROR);
      {
        int soft = special_soft;
        special_soft = 0;
        status = flush_builtin(name, bi->fn(argc, argv));
        special_soft = soft;
      }
      redir_pop();
      break;

    case K_FUNC:
    case K_BUILTIN:
      redir_push();
      if (apply_redirs(n->redirs, 1) < 0) {
        redir_pop();
        status = 1;
        break;
      }
      if (nassigns) {
        var_scope_push();
        /* an assignment error is fatal for every kind of command */
        if (assign_all(assigns, nassigns, V_EXPORT, 1) < 0)
          raise_exception(EX_ERROR);
      }
      if (kind == K_FUNC) {
        status = callfunction(fn, argc, argv, flags);
      } else {
        /* a special built-in reached through `command` loses its special
         * error handling */
        int soft = special_soft;
        special_soft = bi->special;
        status = flush_builtin(name, bi->fn(argc, argv));
        special_soft = soft;
      }
      if (nassigns) var_scope_pop();
      redir_pop();
      break;

    case K_EXTERNAL:
      if ((flags & EV_EXIT) && !trap_exit_set()) {
        /* we are a forked child with nothing left to do: exec directly */
        if (apply_redirs(n->redirs, 0) < 0) _exit(1);
        if (assign_all(assigns, nassigns, V_EXPORT, 0) < 0) _exit(1);
        shellexec(path, argv, var_environ());
      }
      redir_push();
      if (apply_redirs(n->redirs, 1) < 0) {
        redir_pop();
        status = 1;
        break;
      }
      if (nassigns) {
        var_scope_push();
        if (assign_all(assigns, nassigns, V_EXPORT, 1) < 0)
          raise_exception(EX_ERROR);
      }
      fflush(stdout);
      pid_t pid;
      int err = spawn(path, argv, &pid);
      if (err == ENOENT && !strchr(name, '/') && !search_path) {
        /* stale hash entry: search again */
        hash_remove(name);
        path = hash_get(name);
        err = path ? spawn(path, argv, &pid) : ENOENT;
      }
      if (nassigns) var_scope_pop();
      if (err) {
        sh_warn("%s: %s", name, err == ENOENT ? "not found" : strerror(err));
        redir_pop();
        status = err == ENOENT ? 127 : 126;
        break;
      }
      redir_pop();
      status = waitforpid(pid);
      break;

    default:
      /* like a child would, report it under the command's redirections */
      redir_push();
      if (apply_redirs(n->redirs, 1) == 0) sh_warn("%s: not found", name);
      redir_pop();
      status = 127;
  }
  strelease(mark);
  return status;
}

/* ---- compound commands ---- */

/* Handle break/continue after a loop iteration; returns 1 to leave the
 * loop. */
static int loop_skip(void) {
  if (evalskip == SKIP_BREAK) {
    if (--skipcount <= 0) evalskip = SKIP_NONE;
    return 1;
  }
  if (evalskip == SKIP_CONT) {
    if (--skipcount <= 0) {
      evalskip = SKIP_NONE;
      return 0;
    }
    return 1;
  }
  return 1; /* return */
}

static int evalloop(struct node *n, int flags) {
  int status = 0;
  loopnest++;
  for (;;) {
    int c = evaltree(n->u.loop.cond, EV_TESTED);
    if (evalskip) {
      if (loop_skip()) break;
      continue;
    }
    if (n->type == N_WHILE ? c != 0 : c == 0) break;
    status = evaltree(n->u.loop.body, flags & EV_TESTED);
    if (evalskip && loop_skip()) break;
  }
  loopnest--;
  return status;
}

static int evalfor(struct node *n, int flags) {
  stackmark mark = stmark();
  struct arglist items;
  arglist_init(&items);
  if (n->u.forn.words) {
    for (int i = 0; i < n->u.forn.nwords; i++)
      expand_fields(n->u.forn.words[i], &items);
  } else {
    for (int i = 0; i < pos.argc; i++) arglist_add(&items, ststrdup(pos.argv[i]));
  }
  int status = 0;
  loopnest++;
  for (int i = 0; i < items.n; i++) {
    if (var_set(n->u.forn.var, items.v[i], 0) < 0) raise_exception(EX_ERROR);
    status = evaltree(n->u.forn.body, flags & EV_TESTED);
    if (evalskip && loop_skip()) break;
  }
  loopnest--;
  strelease(mark);
  return status;
}

static int evalcase(struct node *n, int flags) {
  stackmark mark = stmark();
  const char *word = expand_str(n->u.casen.word, 0);
  int status = 0;
  for (struct caseitem *ci = n->u.casen.items; ci; ci = ci->next) {
    for (int i = 0; i < ci->npats; i++) {
      if (fnmatch(expand_pattern(ci->pats[i]), word, 0) == 0) {
        status = ci->body ? evaltree(ci->body, flags) : 0;
        strelease(mark);
        return status;
      }
    }
  }
  strelease(mark);
  return status;
}

static int evalpipe(struct node *n, int flags) {
  int cnt = n->u.list.n;
  pid_t *pids = xmalloc((size_t)cnt * sizeof(pid_t));
  int prev = -1;
  for (int i = 0; i < cnt; i++) {
    int pfd[2] = {-1, -1};
    if (i < cnt - 1 && pipe(pfd) < 0) {
      int e = errno;
      if (prev >= 0) close(prev);
      for (int j = 0; j < i; j++) waitforpid(pids[j]);
      free(pids);
      sh_error("pipe: %s", strerror(e));
    }
    pid_t pid = forkshell(0);
    if (pid == 0) {
      free(pids);
      if (prev >= 0) {
        dup2(prev, 0);
        close(prev);
      }
      if (pfd[1] >= 0) {
        close(pfd[0]);
        if (pfd[1] != 1) {
          dup2(pfd[1], 1);
          close(pfd[1]);
        }
      }
      evaltree(n->u.list.items[i], EV_EXIT);
    }
    pids[i] = pid;
    if (prev >= 0) close(prev);
    if (pfd[1] >= 0) {
      close(pfd[1]);
      prev = pfd[0];
    }
  }
  int status = 0, failed = 0;
  for (int i = 0; i < cnt; i++) {
    int st = waitforpid(pids[i]);
    if (st) failed = st;
    if (i == cnt - 1) status = st;
  }
  free(pids);
  (void)flags;
  /* pipefail: the last non-zero status wins */
  return optval[OPT_pipefail] && failed ? failed : status;
}

static int evalnode(struct node *n, int flags) {
  int status = 0;
  switch (n->type) {
    case N_SIMPLE:
      status = evalcommand(n, flags);
      break;
    case N_LIST:
      for (int i = 0; i < n->u.list.n; i++) {
        int last = i == n->u.list.n - 1;
        status = evaltree(n->u.list.items[i], last ? flags : flags & ~EV_EXIT);
        if (evalskip) break;
      }
      break;
    case N_AND:
    case N_OR:
      status = evaltree(n->u.bin.left, (flags & ~EV_EXIT) | EV_TESTED);
      if (evalskip) break;
      if ((status == 0) == (n->type == N_AND))
        status = evaltree(n->u.bin.right, flags);
      break;
    case N_NOT:
      status = !evaltree(n->u.body, (flags & ~EV_EXIT) | EV_TESTED);
      break;
    case N_PIPE:
      status = evalpipe(n, flags);
      break;
    case N_BG: {
      pid_t pid = forkshell(1);
      if (pid == 0) evaltree(n->u.body, EV_EXIT);
      backgndpid = pid;
      jobs_add(pid);
      status = 0;
      break;
    }
    case N_SUBSHELL:
      if (flags & EV_EXIT) {
        if (apply_redirs(n->redirs, 0) < 0) _exit(1);
        evaltree(n->u.body, EV_EXIT);
      } else {
        pid_t pid = forkshell(0);
        if (pid == 0) {
          if (apply_redirs(n->redirs, 0) < 0) _exit(1);
          evaltree(n->u.body, EV_EXIT);
        }
        status = waitforpid(pid);
      }
      break;
    case N_GROUP:
      status = evaltree(n->u.body, flags);
      break;
    case N_IF: {
      int c = evaltree(n->u.ifn.cond, EV_TESTED);
      if (evalskip) break;
      if (c == 0)
        status = evaltree(n->u.ifn.then, flags);
      else if (n->u.ifn.els)
        status = evaltree(n->u.ifn.els, flags);
      else
        status = 0;
      break;
    }
    case N_WHILE:
    case N_UNTIL:
      status = evalloop(n, flags);
      break;
    case N_FOR:
      status = evalfor(n, flags);
      break;
    case N_CASE:
      status = evalcase(n, flags);
      break;
    case N_FUNCDEF:
      func_define(n->u.func.name, n->u.func.body, n->u.func.a);
      status = 0;
      break;
  }
  return status;
}

int evaltree(struct node *n, int flags) {
  int status = 0;
  if (nflag && !iflag) n = NULL;
  if (n) {
    if (n->redirs && n->type != N_SIMPLE && n->type != N_SUBSHELL) {
      redir_push();
      if (apply_redirs(n->redirs, 1) < 0) {
        status = 1;
      } else {
        status = evalnode(n, flags & ~EV_EXIT);
      }
      redir_pop();
    } else {
      status = evalnode(n, flags);
    }
  }
  exitstatus = status;
  if (pending_traps) dotrap();
  if (eflag && status && !(flags & EV_TESTED) && !evalskip && n &&
      (n->type == N_SIMPLE || n->type == N_PIPE || n->type == N_SUBSHELL))
    shell_exit(status);
  if (flags & EV_EXIT) shell_exit(exitstatus);
  return status;
}

/* ---- running text ---- */

static int rest_is_blank(struct source *src) {
  if (src->push || src->nunget) return 0;
  for (const char *p = src->str + src->strpos; *p; p++)
    if (!strchr(" \t\n", *p)) return 0;
  return 1;
}

int evalstring(const char *s, int flags) {
  struct source *src = src_string(s);
  int status = 0;
  for (;;) {
    arena *a;
    int eof;
    struct node *n = parse_command(src, &a, &eof);
    if (eof) {
      arena_unref(a);
      break;
    }
    if (n) {
      int last = (flags & EV_EXIT) && rest_is_blank(src);
      status = evaltree(n, last ? flags : flags & ~EV_EXIT);
    }
    arena_unref(a);
    if (evalskip) break;
  }
  src_free(src);
  if (flags & EV_EXIT) shell_exit(exitstatus);
  return status;
}

int evalsource(struct source *src, int toplevel) {
  for (;;) {
    stackmark mark = stmark();
    if (toplevel) {
      if (iflag) jobs_reap();
      if (pending_traps) dotrap();
    }
    arena *a;
    int eof;
    /* at top level the arena is published before parsing, so an error
     * unwinding out of the parser can still release it */
    struct node *n = parse_command(src, toplevel ? &toplevel_arena : &a, &eof);
    if (toplevel) a = toplevel_arena;
    if (toplevel && src->interactive && src->hist.len) {
      history_add(src->hist.s);
      src->hist.len = 0;
    }
    if (eof) {
      arena_unref(a);
      strelease(mark);
      if (toplevel) toplevel_arena = NULL;
      if (toplevel && src->interactive && optval[OPT_ignoreeof]) {
        fputs("\nUse \"exit\" to leave the shell.\n", stderr);
        src->eof = 0;
        continue;
      }
      break;
    }
    if (n && (!nflag || iflag)) {
      src_sync(src);
      evaltree(n, 0);
    }
    arena_unref(a);
    if (toplevel) toplevel_arena = NULL;
    strelease(mark);
    if (evalskip) break;
  }
  return exitstatus;
}
