/* test / [ : evaluate conditional expressions (POSIX algorithm). */
#include "builtins.h"
#include "common.h"

static jmp_buf test_jmp;

static NORETURN void test_error(const char *fmt, const char *arg) {
  fflush(stdout);
  fprintf(stderr, "%s: test: ", progname);
  fprintf(stderr, fmt, arg);
  fputc('\n', stderr);
  longjmp(test_jmp, 1);
}

static long to_int(const char *s) {
  char *end;
  while (*s == ' ' || *s == '\t') s++;
  errno = 0;
  long v = strtol(s, &end, 10);
  while (*end == ' ' || *end == '\t') end++;
  if (end == s || *end || errno) test_error("%s: integer expected", s);
  return v;
}

static int is_unary(const char *op) {
  return op[0] == '-' && op[1] && !op[2] &&
         strchr("bcdefghkLnprSstuwxz", op[1]);
}

static int is_binary(const char *op) {
  static const char *const ops[] = {"=",   "!=",  "-eq", "-ne", "-gt", "-ge",
                                    "-lt", "-le", "-nt", "-ot", "-ef", "<",
                                    ">",   "==",  NULL};
  for (int i = 0; ops[i]; i++)
    if (strcmp(op, ops[i]) == 0) return 1;
  return 0;
}

static int unary(const char *op, const char *arg) {
  struct stat st;
  switch (op[1]) {
    case 'n':
      return *arg != '\0';
    case 'z':
      return *arg == '\0';
    case 't':
      return isatty((int)to_int(arg));
    case 'r':
      return access(arg, R_OK) == 0;
    case 'w':
      return access(arg, W_OK) == 0;
    case 'x':
      return access(arg, X_OK) == 0;
    case 'h':
    case 'L':
      return lstat(arg, &st) == 0 && S_ISLNK(st.st_mode);
  }
  if (stat(arg, &st) != 0) return 0;
  switch (op[1]) {
    case 'b':
      return S_ISBLK(st.st_mode);
    case 'c':
      return S_ISCHR(st.st_mode);
    case 'd':
      return S_ISDIR(st.st_mode);
    case 'e':
      return 1;
    case 'f':
      return S_ISREG(st.st_mode);
    case 'g':
      return (st.st_mode & S_ISGID) != 0;
    case 'k':
      return (st.st_mode & S_ISVTX) != 0;
    case 'p':
      return S_ISFIFO(st.st_mode);
    case 'S':
      return S_ISSOCK(st.st_mode);
    case 's':
      return st.st_size > 0;
    case 'u':
      return (st.st_mode & S_ISUID) != 0;
  }
  return 0;
}

static int newer(const struct stat *a, const struct stat *b) {
#if defined(__APPLE__)
  if (a->st_mtimespec.tv_sec != b->st_mtimespec.tv_sec)
    return a->st_mtimespec.tv_sec > b->st_mtimespec.tv_sec;
  return a->st_mtimespec.tv_nsec > b->st_mtimespec.tv_nsec;
#else
  if (a->st_mtim.tv_sec != b->st_mtim.tv_sec)
    return a->st_mtim.tv_sec > b->st_mtim.tv_sec;
  return a->st_mtim.tv_nsec > b->st_mtim.tv_nsec;
#endif
}

static int binary(const char *l, const char *op, const char *r) {
  if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) return strcmp(l, r) == 0;
  if (strcmp(op, "!=") == 0) return strcmp(l, r) != 0;
  if (strcmp(op, "<") == 0) return strcoll(l, r) < 0;
  if (strcmp(op, ">") == 0) return strcoll(l, r) > 0;
  if (strcmp(op, "-nt") == 0 || strcmp(op, "-ot") == 0 ||
      strcmp(op, "-ef") == 0) {
    struct stat a, b;
    int ha = stat(l, &a) == 0, hb = stat(r, &b) == 0;
    if (op[1] == 'n') return ha && (!hb || newer(&a, &b));
    if (op[1] == 'o') return hb && (!ha || newer(&b, &a));
    return ha && hb && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
  }
  long a = to_int(l), b = to_int(r);
  if (strcmp(op, "-eq") == 0) return a == b;
  if (strcmp(op, "-ne") == 0) return a != b;
  if (strcmp(op, "-gt") == 0) return a > b;
  if (strcmp(op, "-ge") == 0) return a >= b;
  if (strcmp(op, "-lt") == 0) return a < b;
  return a <= b; /* -le */
}

/* Recursive parser for the general (more than four arguments) case. */
static char **ta;
static int tn, ti;

static int t_or(void);

static int t_primary(void) {
  if (ti >= tn) test_error("%s", "argument expected");
  const char *a = ta[ti];
  if (strcmp(a, "!") == 0) {
    ti++;
    return !t_primary();
  }
  if (strcmp(a, "(") == 0) {
    ti++;
    int v = t_or();
    if (ti >= tn || strcmp(ta[ti], ")") != 0) test_error("%s", "missing )");
    ti++;
    return v;
  }
  if (ti + 2 < tn && is_binary(ta[ti + 1])) {
    int v = binary(a, ta[ti + 1], ta[ti + 2]);
    ti += 3;
    return v;
  }
  if (is_unary(a) && ti + 1 < tn) {
    int v = unary(a, ta[ti + 1]);
    ti += 2;
    return v;
  }
  ti++;
  return *a != '\0';
}

static int t_and(void) {
  int v = t_primary();
  while (ti < tn && strcmp(ta[ti], "-a") == 0) {
    ti++;
    int r = t_primary();
    v = v && r;
  }
  return v;
}

static int t_or(void) {
  int v = t_and();
  while (ti < tn && strcmp(ta[ti], "-o") == 0) {
    ti++;
    int r = t_and();
    v = v || r;
  }
  return v;
}

static int eval_n(char **a, int n) {
  switch (n) {
    case 0:
      return 0;
    case 1:
      return *a[0] != '\0';
    case 2:
      if (strcmp(a[0], "!") == 0) return !eval_n(a + 1, 1);
      if (is_unary(a[0])) return unary(a[0], a[1]);
      break;
    case 3:
      if (is_binary(a[1])) return binary(a[0], a[1], a[2]);
      if (strcmp(a[0], "!") == 0) return !eval_n(a + 1, 2);
      if (strcmp(a[0], "(") == 0 && strcmp(a[2], ")") == 0)
        return eval_n(a + 1, 1);
      break;
    case 4:
      if (strcmp(a[0], "!") == 0) return !eval_n(a + 1, 3);
      if (strcmp(a[0], "(") == 0 && strcmp(a[3], ")") == 0)
        return eval_n(a + 1, 2);
      break;
  }
  ta = a;
  tn = n;
  ti = 0;
  int v = t_or();
  if (ti < tn) test_error("%s: unexpected operator", ta[ti]);
  return v;
}

int test_builtin(int argc, char **argv) {
  if (strcmp(argv[0], "[") == 0) {
    if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
      sh_warn("[: missing ]");
      return 2;
    }
    argc--;
  }
  char **volatile args = argv + 1;
  volatile int n = argc - 1;
  if (setjmp(test_jmp)) return 2;
  return eval_n(args, n) ? 0 : 1;
}
