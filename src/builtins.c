#include "builtins.h"

#include <sys/times.h>

#include "alias.h"
#include "exec.h"
#include "expand.h"
#include "history.h"
#include "input.h"
#include "shell.h"
#include "trap.h"
#include "vars.h"

static int bad_usage(const char *name, const char *msg) {
  sh_warn("%s: %s", name, msg);
  return 2;
}

/* ---- trivial ---- */

static int colon_builtin(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return 0;
}

static int false_builtin(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return 1;
}

/* XSI echo: -n suppresses the newline, backslash escapes are honoured. */
static int echo_builtin(int argc, char **argv) {
  int i = 1, newline = 1;
  while (i < argc && strcmp(argv[i], "-n") == 0) {
    newline = 0;
    i++;
  }
  strbuf sb;
  sb_init(&sb);
  for (; i < argc; i++) {
    for (const char *p = argv[i]; *p; p++) {
      if (*p != '\\' || !p[1]) {
        sb_putc(&sb, *p);
        continue;
      }
      p++;
      switch (*p) {
        case 'a': sb_putc(&sb, '\a'); break;
        case 'b': sb_putc(&sb, '\b'); break;
        case 'c':
          fwrite(sb.s, 1, sb.len, stdout);
          sb_free(&sb);
          return 0;
        case 'f': sb_putc(&sb, '\f'); break;
        case 'n': sb_putc(&sb, '\n'); break;
        case 'r': sb_putc(&sb, '\r'); break;
        case 't': sb_putc(&sb, '\t'); break;
        case 'v': sb_putc(&sb, '\v'); break;
        case '\\': sb_putc(&sb, '\\'); break;
        case '0': {
          int v = 0, k = 0;
          while (k < 3 && p[1] >= '0' && p[1] <= '7') {
            v = v * 8 + (*++p - '0');
            k++;
          }
          sb_putc(&sb, (char)v);
          break;
        }
        default:
          sb_putc(&sb, '\\');
          sb_putc(&sb, *p);
      }
    }
    if (i + 1 < argc) sb_putc(&sb, ' ');
  }
  if (newline) sb_putc(&sb, '\n');
  if (sb.len) fwrite(sb.s, 1, sb.len, stdout);
  sb_free(&sb);
  return 0;
}

/* ---- directories ---- */

/* Lexically resolve . and .. in an absolute path. */
static char *canonicalize(const char *path) {
  size_t n = strlen(path);
  char *out = xmalloc(n + 2);
  size_t len = 0;
  const char *p = path;
  while (*p) {
    while (*p == '/') p++;
    const char *e = p;
    while (*e && *e != '/') e++;
    size_t cl = (size_t)(e - p);
    if (cl == 0) break;
    if (cl == 1 && p[0] == '.') {
      /* skip */
    } else if (cl == 2 && p[0] == '.' && p[1] == '.') {
      while (len > 0 && out[len - 1] != '/') len--;
      if (len > 0) len--;
    } else {
      out[len++] = '/';
      memcpy(out + len, p, cl);
      len += cl;
    }
    p = e;
  }
  if (len == 0) out[len++] = '/';
  out[len] = '\0';
  return out;
}

static int do_chdir(const char *dir, int physical, int print) {
  char *target;
  const char *pwd = var_get("PWD");
  if (physical) {
    target = NULL;
  } else if (dir[0] == '/') {
    target = canonicalize(dir);
  } else if (pwd && pwd[0] == '/') {
    size_t n = strlen(pwd) + strlen(dir) + 2;
    char *joined = xmalloc(n);
    snprintf(joined, n, "%s/%s", pwd, dir);
    target = canonicalize(joined);
    free(joined);
  } else {
    target = NULL;
    physical = 1;
  }
  if (chdir(target ? target : dir) != 0) {
    /* a logical path through a symlinked parent can fail: retry as-is */
    if (!target || chdir(dir) != 0) {
      free(target);
      return -1;
    }
    free(target);
    target = NULL;
    physical = 1;
  }
  char buf[PATH_MAX];
  char *oldpwd = pwd ? xstrdup(pwd) : NULL;
  const char *newpwd = target;
  if (physical || !newpwd) newpwd = getcwd(buf, sizeof(buf)) ? buf : dir;
  var_set("OLDPWD", oldpwd ? oldpwd : "", V_EXPORT);
  var_set("PWD", newpwd, V_EXPORT);
  if (print) puts(newpwd);
  free(oldpwd);
  free(target);
  return 0;
}

static int cd_builtin(int argc, char **argv) {
  int physical = 0, i = 1;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    if (strcmp(argv[i], "-L") == 0)
      physical = 0;
    else if (strcmp(argv[i], "-P") == 0)
      physical = 1;
    else
      return bad_usage("cd", "usage: cd [-L|-P] [directory]");
  }
  const char *dir = i < argc ? argv[i] : NULL;
  int print = 0;
  if (!dir) {
    dir = var_get("HOME");
    if (!dir || !*dir) {
      sh_warn("cd: HOME not set");
      return 1;
    }
  } else if (strcmp(dir, "-") == 0) {
    dir = var_get("OLDPWD");
    if (!dir || !*dir) {
      sh_warn("cd: OLDPWD not set");
      return 1;
    }
    print = 1;
  }
  /* CDPATH applies to relative names not starting with . or .. */
  const char *cdpath = var_get("CDPATH");
  int dotted = dir[0] == '.' &&
               (dir[1] == '\0' || dir[1] == '/' ||
                (dir[1] == '.' && (dir[2] == '\0' || dir[2] == '/')));
  if (cdpath && dir[0] != '/' && !dotted) {
    for (const char *p = cdpath;;) {
      const char *colon = strchr(p, ':');
      size_t dl = colon ? (size_t)(colon - p) : strlen(p);
      size_t n = dl + strlen(dir) + 3;
      char *cand = xmalloc(n);
      if (dl)
        snprintf(cand, n, "%.*s/%s", (int)dl, p, dir);
      else
        snprintf(cand, n, "./%s", dir);
      struct stat st;
      if (stat(cand, &st) == 0 && S_ISDIR(st.st_mode) &&
          do_chdir(cand, physical, print || dl > 0) == 0) {
        free(cand);
        return 0;
      }
      free(cand);
      if (!colon) break;
      p = colon + 1;
    }
  }
  if (do_chdir(dir, physical, print) != 0) {
    sh_warn("cd: %s: %s", dir, strerror(errno));
    return 1;
  }
  return 0;
}

static int pwd_builtin(int argc, char **argv) {
  int physical = 0;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-P") == 0)
      physical = 1;
    else if (strcmp(argv[i], "-L") == 0)
      physical = 0;
    else
      return bad_usage("pwd", "usage: pwd [-L|-P]");
  }
  const char *pwd = var_get("PWD");
  struct stat a, b;
  if (!physical && pwd && pwd[0] == '/' && stat(pwd, &a) == 0 &&
      stat(".", &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino) {
    puts(pwd);
    return 0;
  }
  char buf[PATH_MAX];
  if (!getcwd(buf, sizeof(buf))) {
    sh_warn("pwd: %s", strerror(errno));
    return 1;
  }
  puts(buf);
  return 0;
}

/* ---- special built-ins ---- */

static int exit_builtin(int argc, char **argv) {
  int status = exitstatus;
  if (argc > 1) {
    if (!is_number(argv[1]) &&
        !(argv[1][0] == '-' && is_number(argv[1] + 1)))
      sh_error("exit: %s: numeric argument required", argv[1]);
    status = atoi(argv[1]);
  }
  shell_exit(status & 255);
}

static int set_vars(int argc, char **argv, int flag, const char *name) {
  int i = 1, status = 0;
  if (i < argc && strcmp(argv[i], "-p") == 0) i++;
  if (i < argc && strcmp(argv[i], "--") == 0) i++;
  if (i >= argc) {
    var_print(flag, flag == V_EXPORT ? "export " : "readonly ");
    return 0;
  }
  for (; i < argc; i++) {
    const char *eq = strchr(argv[i], '=');
    size_t n = eq ? (size_t)(eq - argv[i]) : strlen(argv[i]);
    if (name_len(argv[i]) != n || n == 0) {
      sh_warn("%s: %s: bad variable name", name, argv[i]);
      status = 1;
      continue;
    }
    if (var_set_n(argv[i], n, eq ? eq + 1 : NULL, flag) < 0) status = 1;
  }
  return status;
}

static int export_builtin(int argc, char **argv) {
  return set_vars(argc, argv, V_EXPORT, "export");
}

static int readonly_builtin(int argc, char **argv) {
  return set_vars(argc, argv, V_READONLY, "readonly");
}

static int unset_builtin(int argc, char **argv) {
  int funcs = 0, i = 1, status = 0;
  for (; i < argc && argv[i][0] == '-'; i++) {
    if (strcmp(argv[i], "-f") == 0)
      funcs = 1;
    else if (strcmp(argv[i], "-v") == 0)
      funcs = 0;
    else if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    } else
      return bad_usage("unset", "usage: unset [-f|-v] name...");
  }
  for (; i < argc; i++) {
    if (funcs)
      func_unset(argv[i]);
    else if (var_unset(argv[i]) < 0)
      status = 1;
  }
  return status;
}

static void print_options(int restorable) {
  for (int i = 0; i < NOPTS; i++) {
    if (restorable)
      printf("set %co %s\n", optval[i] ? '-' : '+', optdefs[i].name);
    else
      printf("%-15s %s\n", optdefs[i].name, optval[i] ? "on" : "off");
  }
}

static int set_builtin(int argc, char **argv) {
  if (argc == 1) {
    var_print(0, "");
    return 0;
  }
  int i = 1;
  for (; i < argc; i++) {
    const char *a = argv[i];
    if ((a[0] != '-' && a[0] != '+') || !a[1]) {
      if (strcmp(a, "-") == 0) {
        /* historical: turn off -x and -v, end of options */
        optval[OPT_x] = optval[OPT_v] = 0;
        i++;
      }
      break;
    }
    if (strcmp(a, "--") == 0) {
      i++;
      pos_set(argc - i, argv + i);
      return 0;
    }
    int on = a[0] == '-';
    for (const char *p = a + 1; *p; p++) {
      if (*p == 'o') {
        if (i + 1 >= argc || argv[i + 1][0] == '-' || argv[i + 1][0] == '+') {
          print_options(!on);
          continue;
        }
        if (setoption_name(argv[++i], on) < 0) {
          sh_warn("set: %s: invalid option name", argv[i]);
          return 2;
        }
      } else if (*p == 'i' || *p == 's' || setoption_letter(*p, on) < 0) {
        sh_warn("set: -%c: invalid option", *p);
        return 2;
      }
    }
  }
  if (i < argc) pos_set(argc - i, argv + i);
  return 0;
}

static int shift_builtin(int argc, char **argv) {
  int n = 1;
  if (argc > 1) {
    if (!is_number(argv[1])) sh_error("shift: %s: bad number", argv[1]);
    n = atoi(argv[1]);
  }
  if (n > pos.argc) {
    sh_warn("shift: can't shift that many");
    return 1;
  }
  for (int i = 0; i < n; i++) free(pos.argv[i]);
  memmove(pos.argv, pos.argv + n, (size_t)(pos.argc - n + 1) * sizeof(char *));
  pos.argc -= n;
  return 0;
}

static int eval_builtin(int argc, char **argv) {
  if (argc < 2) return 0;
  strbuf sb;
  sb_init(&sb);
  for (int i = 1; i < argc; i++) {
    if (i > 1) sb_putc(&sb, ' ');
    sb_puts(&sb, argv[i]);
  }
  char *s = sb_detach(&sb);
  evalstring(s, 0);
  free(s);
  return exitstatus;
}

static int dot_builtin(int argc, char **argv) {
  int i = 1;
  if (i < argc && strcmp(argv[i], "--") == 0) i++;
  if (i >= argc) sh_error(".: filename argument required");
  const char *name = argv[i];
  char *path = NULL;
  if (!strchr(name, '/')) {
    const char *p = var_get("PATH");
    for (const char *s = p ? p : "";;) {
      const char *colon = strchr(s, ':');
      size_t dl = colon ? (size_t)(colon - s) : strlen(s);
      size_t n = dl + strlen(name) + 3;
      char *cand = xmalloc(n);
      snprintf(cand, n, "%.*s/%s", (int)(dl ? dl : 1), dl ? s : ".", name);
      struct stat st;
      if (stat(cand, &st) == 0 && S_ISREG(st.st_mode) &&
          access(cand, R_OK) == 0) {
        path = cand;
        break;
      }
      free(cand);
      if (!colon) break;
      s = colon + 1;
    }
  }
  int fd = open(path ? path : name, O_RDONLY);
  free(path);
  if (fd < 0) sh_error(".: %s: %s", name, strerror(errno));
  fd = move_fd_high(fd);
  struct source *src = src_fd(fd, 0);
  exitstatus = 0;
  dotnest++;
  evalsource(src, 0);
  dotnest--;
  src_free(src);
  close(fd);
  if (evalskip == SKIP_RETURN) evalskip = SKIP_NONE;
  return exitstatus;
}

int exec_builtin(int argc, char **argv) {
  redir_commit();
  int i = 1;
  if (i < argc && strcmp(argv[i], "--") == 0) i++;
  if (i >= argc) return 0;
  const char *name = argv[i];
  const char *path = strchr(name, '/') ? name : hash_get(name);
  if (!path) {
    sh_warn("exec: %s: not found", name);
    if (!iflag) shell_exit(127);
    return 127;
  }
  /* restore default dispositions the shell changed for itself */
  const sigset_t *defs = trap_child_defaults();
  for (int s = 1; s < 65; s++)
    if (sigismember(defs, s) == 1) signal(s, SIG_DFL);
  fflush(stdout);
  if (!iflag) shellexec(path, argv + i, var_environ());
  /* an interactive shell survives a failed exec */
  execve(path, argv + i, var_environ());
  int e = errno;
  sh_warn("exec: %s: %s", name, strerror(e));
  return e == ENOENT ? 127 : 126;
}

static int loopctl(int argc, char **argv, int kind) {
  int n = 1;
  if (argc > 1) {
    if (!is_number(argv[1]) || atoi(argv[1]) < 1)
      sh_error("%s: %s: bad number", argv[0], argv[1]);
    n = atoi(argv[1]);
  }
  if (loopnest == 0) return 0;
  if (n > loopnest) n = loopnest;
  evalskip = kind;
  skipcount = n;
  return 0;
}

static int break_builtin(int argc, char **argv) {
  return loopctl(argc, argv, SKIP_BREAK);
}

static int continue_builtin(int argc, char **argv) {
  return loopctl(argc, argv, SKIP_CONT);
}

static int return_builtin(int argc, char **argv) {
  int status = exitstatus;
  if (argc > 1) {
    if (!is_number(argv[1]) && !(argv[1][0] == '-' && is_number(argv[1] + 1)))
      sh_error("return: %s: bad number", argv[1]);
    status = atoi(argv[1]) & 255;
  }
  if (funcnest == 0 && dotnest == 0) {
    sh_warn("return: not in a function or sourced script");
    return 1;
  }
  evalskip = SKIP_RETURN;
  exitstatus = status;
  return status;
}

static void print_time(clock_t t, long hz) {
  long ms = (long)(t * 1000 / hz);
  printf("%ldm%ld.%03lds", ms / 60000, (ms / 1000) % 60, ms % 1000);
}

static int times_builtin(int argc, char **argv) {
  (void)argc;
  (void)argv;
  struct tms t;
  long hz = sysconf(_SC_CLK_TCK);
  times(&t);
  print_time(t.tms_utime, hz);
  putchar(' ');
  print_time(t.tms_stime, hz);
  putchar('\n');
  print_time(t.tms_cutime, hz);
  putchar(' ');
  print_time(t.tms_cstime, hz);
  putchar('\n');
  return 0;
}

/* ---- regular built-ins ---- */

static int alias_builtin(int argc, char **argv) {
  if (argc == 1) {
    alias_print_all();
    return 0;
  }
  int status = 0;
  for (int i = 1; i < argc; i++) {
    char *eq = strchr(argv[i], '=');
    if (eq) {
      char *name = xstrndup(argv[i], (size_t)(eq - argv[i]));
      alias_set(name, eq + 1);
      free(name);
    } else {
      struct alias *a = alias_lookup(argv[i]);
      if (a) {
        alias_print(a);
      } else {
        sh_warn("alias: %s: not found", argv[i]);
        status = 1;
      }
    }
  }
  return status;
}

static int unalias_builtin(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "-a") == 0) {
    alias_unset_all();
    return 0;
  }
  int status = 0;
  for (int i = 1; i < argc; i++)
    if (alias_unset(argv[i]) < 0) {
      sh_warn("unalias: %s: not found", argv[i]);
      status = 1;
    }
  return status;
}

static int is_ifs_ws(char c, const char *ifs) {
  return (c == ' ' || c == '\t' || c == '\n') && strchr(ifs, c);
}

static int read_builtin(int argc, char **argv) {
  int raw = 0, i = 1;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-r") == 0)
      raw = 1;
    else if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    } else
      return bad_usage("read", "usage: read [-r] var...");
  }
  if (i >= argc) return bad_usage("read", "variable name required");
  for (int k = i; k < argc; k++)
    if (!valid_name(argv[k])) {
      sh_warn("read: %s: bad variable name", argv[k]);
      return 2;
    }

  /* read one line; escaped characters are marked so they never split */
  strbuf line, esc;
  sb_init(&line);
  sb_init(&esc);
  int status = 0;
  fflush(stdout);
  for (;;) {
    char c;
    ssize_t n = read(0, &c, 1);
    if (n < 0 && errno == EINTR) {
      if (pending_traps) {
        sb_free(&line);
        sb_free(&esc);
        return 128 + SIGINT;
      }
      continue;
    }
    if (n <= 0) {
      status = 1;
      break;
    }
    if (c == '\n') break;
    if (c == '\\' && !raw) {
      n = read(0, &c, 1);
      if (n <= 0) {
        status = 1;
        break;
      }
      if (c == '\n') continue; /* line continuation */
      sb_putc(&line, c);
      sb_putc(&esc, 1);
      continue;
    }
    sb_putc(&line, c);
    sb_putc(&esc, 0);
  }
  const char *ifs = var_get("IFS");
  if (!ifs) ifs = " \t\n";
  const char *s = line.s ? line.s : "";
  const char *e = esc.s ? esc.s : "";
  size_t len = line.len, p = 0;
  while (p < len && !e[p] && is_ifs_ws(s[p], ifs)) p++;
  for (; i < argc; i++) {
    if (i == argc - 1) {
      /* last variable: the rest, minus trailing IFS whitespace */
      size_t end = len;
      while (end > p && !e[end - 1] && is_ifs_ws(s[end - 1], ifs)) end--;
      char *v = xstrndup(s + p, end - p);
      if (var_set(argv[i], v, 0) < 0) status = 2;
      free(v);
      break;
    }
    size_t start = p;
    while (p < len && !(!e[p] && strchr(ifs, s[p]))) p++;
    char *v = xstrndup(s + start, p - start);
    if (var_set(argv[i], v, 0) < 0) status = 2;
    free(v);
    /* consume the delimiter */
    while (p < len && !e[p] && is_ifs_ws(s[p], ifs)) p++;
    if (p < len && !e[p] && strchr(ifs, s[p]) && !is_ifs_ws(s[p], ifs)) {
      p++;
      while (p < len && !e[p] && is_ifs_ws(s[p], ifs)) p++;
    }
  }
  sb_free(&line);
  sb_free(&esc);
  return status;
}

static void umask_symbolic(mode_t m) {
  const char *who = "ugo";
  for (int w = 0; w < 3; w++) {
    int shift = 6 - 3 * w;
    mode_t allowed = ~m >> shift;
    printf("%c=%s%s%s%s", who[w], allowed & 4 ? "r" : "", allowed & 2 ? "w" : "",
           allowed & 1 ? "x" : "", w < 2 ? "," : "\n");
  }
}

static int umask_builtin(int argc, char **argv) {
  int symbolic = 0, i = 1;
  if (i < argc && strcmp(argv[i], "-S") == 0) {
    symbolic = 1;
    i++;
  }
  mode_t cur = umask(0);
  umask(cur);
  if (i >= argc) {
    if (symbolic)
      umask_symbolic(cur);
    else
      printf("%04o\n", (unsigned)cur);
    return 0;
  }
  const char *a = argv[i];
  if (isdigit((unsigned char)*a)) {
    char *end;
    long v = strtol(a, &end, 8);
    if (*end || v < 0 || v > 0777) {
      sh_warn("umask: %s: invalid mask", a);
      return 1;
    }
    umask((mode_t)v);
    return 0;
  }
  /* symbolic: [ugoa]*[+-=][rwx]* separated by commas */
  mode_t allowed = ~cur & 0777;
  for (const char *p = a; *p;) {
    mode_t who = 0;
    for (; *p && strchr("ugoa", *p); p++)
      who |= *p == 'u' ? 0700 : *p == 'g' ? 0070 : *p == 'o' ? 0007 : 0777;
    if (!who) who = 0777;
    while (*p && strchr("+-=", *p)) {
      char op = *p++;
      mode_t perm = 0;
      for (; *p && strchr("rwx", *p); p++)
        perm |= *p == 'r' ? 0444 : *p == 'w' ? 0222 : 0111;
      perm &= who;
      if (op == '+')
        allowed |= perm;
      else if (op == '-')
        allowed &= ~perm;
      else
        allowed = (allowed & ~who) | perm;
    }
    if (*p == ',') {
      p++;
    } else if (*p) {
      sh_warn("umask: %s: invalid mask", a);
      return 1;
    }
  }
  umask(~allowed & 0777);
  return 0;
}

static int wait_builtin(int argc, char **argv) {
  int found;
  if (argc < 2) return jobs_wait(0, &found);
  int status = 0;
  for (int i = 1; i < argc; i++) {
    if (!is_number(argv[i])) {
      sh_warn("wait: %s: invalid process id", argv[i]);
      status = 2;
      continue;
    }
    status = jobs_wait((pid_t)atol(argv[i]), &found);
    if (!found) status = 127;
  }
  return status;
}

static const char *const keywords[] = {
    "!",    "{",  "}",     "case", "do",    "done", "elif", "else",
    "esac", "fi", "for",   "if",   "in",    "then", "until", "while", NULL};

/* command -v / -V and type: describe how a name would be run. */
static int describe(const char *name, int verbose) {
  for (int i = 0; keywords[i]; i++)
    if (strcmp(name, keywords[i]) == 0) {
      if (verbose)
        printf("%s is a shell keyword\n", name);
      else
        puts(name);
      return 0;
    }
  struct alias *a = alias_lookup(name);
  if (a) {
    strbuf sb;
    sb_init(&sb);
    sh_quote(&sb, a->value);
    if (verbose)
      printf("%s is an alias for %s\n", name, a->value);
    else
      printf("alias %s=%s\n", name, sb.s);
    sb_free(&sb);
    return 0;
  }
  const struct builtin *b = find_builtin(name);
  if (b && b->special) {
    if (verbose)
      printf("%s is a special shell builtin\n", name);
    else
      puts(name);
    return 0;
  }
  if (func_lookup(name)) {
    if (verbose)
      printf("%s is a function\n", name);
    else
      puts(name);
    return 0;
  }
  if (b) {
    if (verbose)
      printf("%s is a shell builtin\n", name);
    else
      puts(name);
    return 0;
  }
  const char *path = NULL;
  if (strchr(name, '/')) {
    if (access(name, X_OK) == 0) path = name;
  } else {
    path = hash_get(name);
  }
  if (path) {
    if (verbose)
      printf("%s is %s\n", name, path);
    else
      puts(path);
    return 0;
  }
  if (verbose) sh_warn("%s: not found", name);
  return 1;
}

static int command_builtin(int argc, char **argv) {
  int verbose = -1, i = 1;
  for (; i < argc && argv[i][0] == '-'; i++) {
    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    for (const char *p = argv[i] + 1; *p; p++) {
      if (*p == 'v')
        verbose = 0;
      else if (*p == 'V')
        verbose = 1;
      else if (*p != 'p')
        return bad_usage("command", "usage: command [-p] [-v|-V] name");
    }
  }
  if (verbose < 0 || i >= argc) return 0;
  int status = 0;
  for (; i < argc; i++)
    if (describe(argv[i], verbose)) status = 127;
  return status;
}

static int type_builtin(int argc, char **argv) {
  int status = 0;
  for (int i = 1; i < argc; i++)
    if (describe(argv[i], 1)) status = 1;
  return status;
}

static int hash_builtin(int argc, char **argv) {
  if (argc == 1) {
    hash_print();
    return 0;
  }
  int status = 0;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-r") == 0) {
      hash_clear();
      continue;
    }
    if (find_builtin(argv[i]) || func_lookup(argv[i])) continue;
    if (!hash_get(argv[i])) {
      sh_warn("hash: %s: not found", argv[i]);
      status = 1;
    }
  }
  return status;
}

static int getopts_builtin(int argc, char **argv) {
  static int subpos = 1;     /* position inside the current argument */
  static long last_optind = 1;
  if (argc < 3) return bad_usage("getopts", "usage: getopts optstring name [arg...]");
  const char *optstr = argv[1];
  const char *name = argv[2];
  char **args = argc > 3 ? argv + 3 : pos.argv;
  int nargs = argc > 3 ? argc - 3 : pos.argc;
  const char *oi = var_get("OPTIND");
  long optind = oi && is_number(oi) ? atol(oi) : 1;
  if (optind < 1) optind = 1;
  if (optind != last_optind) subpos = 1;
  int silent = optstr[0] == ':';
  char cbuf[2] = {0, 0};
  char nbuf[24];
  int status = 0;
  const char *result = "?";

  if (optind > nargs) {
    status = 1;
  } else {
    const char *arg = args[optind - 1];
    if (subpos == 1 && (arg[0] != '-' || arg[1] == '\0')) {
      status = 1;
    } else if (subpos == 1 && strcmp(arg, "--") == 0) {
      optind++;
      status = 1;
    } else {
      char c = arg[subpos];
      const char *p = c != ':' ? strchr(optstr, c) : NULL;
      cbuf[0] = c;
      int next_arg = arg[subpos + 1] == '\0';
      if (!p) {
        if (silent) {
          var_set("OPTARG", cbuf, 0);
        } else {
          sh_warn("illegal option -- %c", c);
          var_unset("OPTARG");
        }
      } else if (p[1] == ':') {
        if (!next_arg) {
          var_set("OPTARG", arg + subpos + 1, 0);
          next_arg = 1;
          result = cbuf;
        } else if (optind < nargs) {
          var_set("OPTARG", args[optind], 0);
          optind++;
          result = cbuf;
        } else if (silent) {
          var_set("OPTARG", cbuf, 0);
          result = ":";
        } else {
          sh_warn("option requires an argument -- %c", c);
          var_unset("OPTARG");
        }
      } else {
        var_unset("OPTARG");
        result = cbuf;
      }
      if (next_arg) {
        optind++;
        subpos = 1;
      } else {
        subpos++;
      }
    }
  }
  if (status) subpos = 1;
  snprintf(nbuf, sizeof(nbuf), "%ld", optind);
  var_set("OPTIND", nbuf, 0);
  last_optind = optind;
  if (var_set(name, result, 0) < 0) return 2;
  return status;
}

static int local_builtin(int argc, char **argv) {
  if (funcnest == 0) {
    sh_warn("local: not in a function");
    return 1;
  }
  int status = 0;
  for (int i = 1; i < argc; i++) {
    const char *eq = strchr(argv[i], '=');
    char *name = eq ? xstrndup(argv[i], (size_t)(eq - argv[i])) : xstrdup(argv[i]);
    if (!valid_name(name)) {
      sh_warn("local: %s: bad variable name", name);
      status = 1;
    } else if (var_local(name) < 0 || (eq && var_set(name, eq + 1, 0) < 0)) {
      status = 1;
    }
    free(name);
  }
  return status;
}

static int kill_builtin(int argc, char **argv) {
  int sig = SIGTERM, i = 1;
  if (argc < 2) return bad_usage("kill", "usage: kill [-s sig | -sig] pid... | -l [status]");
  if (strcmp(argv[1], "-l") == 0) {
    if (argc > 2) {
      int n = atoi(argv[2]);
      if (n > 128) n -= 128;
      const char *nm = signal_name(n);
      if (!nm) {
        sh_warn("kill: %s: invalid signal", argv[2]);
        return 1;
      }
      puts(nm);
    } else {
      kill_list();
    }
    return 0;
  }
  if (strcmp(argv[1], "-s") == 0) {
    if (argc < 3) return bad_usage("kill", "-s requires a signal name");
    sig = signal_number(argv[2]);
    i = 3;
  } else if (strcmp(argv[1], "--") == 0) {
    i = 2;
  } else if (argv[1][0] == '-') {
    sig = signal_number(argv[1] + 1);
    i = 2;
  }
  if (sig < 0) {
    sh_warn("kill: %s: invalid signal", argv[i - 1]);
    return 1;
  }
  if (i < argc && strcmp(argv[i], "--") == 0) i++;
  int status = 0;
  for (; i < argc; i++) {
    char *end;
    long pid = strtol(argv[i], &end, 10);
    if (*end || end == argv[i]) {
      sh_warn("kill: %s: invalid process id", argv[i]);
      status = 1;
      continue;
    }
    if (kill((pid_t)pid, sig) < 0) {
      sh_warn("kill: %s: %s", argv[i], strerror(errno));
      status = 1;
    }
  }
  return status;
}

static int history_cmd(int argc, char **argv) { return history_builtin(argc, argv); }

/* ---- table ---- */

static const struct builtin table[] = {
    {".", dot_builtin, 1},
    {":", colon_builtin, 1},
    {"[", test_builtin, 0},
    {"alias", alias_builtin, 0},
    {"break", break_builtin, 1},
    {"cd", cd_builtin, 0},
    {"command", command_builtin, 0},
    {"continue", continue_builtin, 1},
    {"echo", echo_builtin, 0},
    {"eval", eval_builtin, 1},
    {"exec", exec_builtin, 1},
    {"exit", exit_builtin, 1},
    {"export", export_builtin, 1},
    {"false", false_builtin, 0},
    {"getopts", getopts_builtin, 0},
    {"hash", hash_builtin, 0},
    {"history", history_cmd, 0},
    {"kill", kill_builtin, 0},
    {"local", local_builtin, 0},
    {"printf", printf_builtin, 0},
    {"pwd", pwd_builtin, 0},
    {"read", read_builtin, 0},
    {"readonly", readonly_builtin, 1},
    {"return", return_builtin, 1},
    {"set", set_builtin, 1},
    {"shift", shift_builtin, 1},
    {"test", test_builtin, 0},
    {"times", times_builtin, 1},
    {"trap", trap_builtin, 1},
    {"true", colon_builtin, 0},
    {"type", type_builtin, 0},
    {"umask", umask_builtin, 0},
    {"unalias", unalias_builtin, 0},
    {"unset", unset_builtin, 1},
    {"wait", wait_builtin, 0},
    {NULL, NULL, 0},
};

const struct builtin *builtin_table(void) { return table; }

const struct builtin *find_builtin(const char *name) {
  /* the table is sorted: binary search */
  size_t lo = 0, hi = sizeof(table) / sizeof(table[0]) - 1;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    int c = strcmp(name, table[mid].name);
    if (c == 0) return &table[mid];
    if (c < 0)
      hi = mid;
    else
      lo = mid + 1;
  }
  return NULL;
}
