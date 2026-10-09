#include <locale.h>

#include "exec.h"
#include "expand.h"
#include "history.h"
#include "input.h"
#include "shell.h"
#include "trap.h"
#include "vars.h"

extern char **environ;

char optval[NOPTS];
const struct optdef optdefs[NOPTS] = {
    {'a', "allexport"}, {'b', "notify"},    {'C', "noclobber"},
    {'e', "errexit"},   {'f', "noglob"},    {'h', "hashall"},
    {'i', "interactive"}, {'m', "monitor"}, {'n', "noexec"},
    {'s', "stdin"},     {'u', "nounset"},   {'v', "verbose"},
    {'x', "xtrace"},    {0, "ignoreeof"},   {0, "nolog"},
    {0, "pipefail"},    {0, "vi"},
};

int exitstatus;
pid_t rootpid;
pid_t backgndpid;
int rootshell = 1;
const char *shell_exe = "/bin/sh";
char *arg0;

int setoption_letter(int letter, int on) {
  for (int i = 0; i < NOPTS; i++)
    if (optdefs[i].letter == letter) {
      optval[i] = (char)on;
      return 0;
    }
  return -1;
}

int setoption_name(const char *name, int on) {
  for (int i = 0; i < NOPTS; i++)
    if (strcmp(optdefs[i].name, name) == 0) {
      if (i == OPT_i || i == OPT_s) return -1;
      optval[i] = (char)on;
      return 0;
    }
  return -1;
}

void options_string(char *buf) {
  size_t k = 0;
  for (int i = 0; i < NOPTS && k < 30; i++)
    if (optval[i] && optdefs[i].letter) buf[k++] = optdefs[i].letter;
  buf[k] = '\0';
}

void shell_exit(int status) {
  exitstatus = status;
  handler = NULL; /* errors from here on just exit */
  run_exit_trap();
  if (rootshell && iflag) history_save();
  setjobctl(0);
  fflush(stdout);
  exit(exitstatus);
}

static NORETURN void usage(void) {
  fprintf(stderr,
          "usage: %s [-abCefhimnuvx] [-o option] [script [arg...]]\n"
          "       %s [options] -c command [name [arg...]]\n"
          "       %s [options] -s [arg...]\n",
          CPSH_NAME, CPSH_NAME, CPSH_NAME);
  exit(2);
}

static const char *find_self(const char *argv0) {
  char buf[PATH_MAX];
  if (strchr(argv0, '/')) {
    if (realpath(argv0, buf)) return xstrdup(buf);
    return xstrdup(argv0);
  }
  char *p = path_lookup(argv0, getenv("PATH"));
  return p ? p : "/bin/sh";
}

static void init_pwd(void) {
  const char *pwd = var_get("PWD");
  struct stat a, b;
  if (pwd && pwd[0] == '/' && stat(pwd, &a) == 0 && stat(".", &b) == 0 &&
      a.st_dev == b.st_dev && a.st_ino == b.st_ino)
    return;
  char buf[PATH_MAX];
  if (getcwd(buf, sizeof(buf))) var_set("PWD", buf, V_EXPORT);
}

static void source_env_file(void) {
  const char *env = var_get("ENV");
  if (!env || !*env) return;
  stackmark m = stmark();
  char *file = expand_str(env, 0);
  int fd = open(file, O_RDONLY);
  strelease(m);
  if (fd < 0) return;
  fd = move_fd_high(fd);
  struct source *src = src_fd(fd, 0);
  evalsource(src, 0);
  src_free(src);
  close(fd);
}

int main(int argc, char **argv) {
  setlocale(LC_ALL, "");
  scratch = arena_new();
  rootpid = getpid();
  vars_init(environ);
  shell_exe = find_self(argv[0]);
  arg0 = argv[0];

  int cflag = 0, iforce = 0, sflag = 0, mgiven = 0, i = 1;
  for (; i < argc; i++) {
    const char *a = argv[i];
    if ((a[0] != '-' && a[0] != '+') || !a[1]) {
      if (strcmp(a, "-") == 0) i++;
      break;
    }
    if (strcmp(a, "--") == 0) {
      i++;
      break;
    }
    int on = a[0] == '-';
    for (const char *p = a + 1; *p; p++) {
      if (*p == 'c' && on) {
        cflag = 1;
      } else if (*p == 'i') {
        iforce = on;
      } else if (*p == 's') {
        sflag = on;
      } else if (*p == 'o') {
        if (i + 1 >= argc) usage();
        if (setoption_name(argv[++i], on) < 0) usage();
        mgiven |= strcmp(argv[i], "monitor") == 0;
      } else if (setoption_letter(*p, on) < 0) {
        fprintf(stderr, "%s: -%c: invalid option\n", CPSH_NAME, *p);
        usage();
      } else if (*p == 'm') {
        mgiven = 1;
      }
    }
  }

  const char *command = NULL, *script = NULL;
  if (cflag) {
    if (i >= argc) {
      fprintf(stderr, "%s: -c: option requires an argument\n", CPSH_NAME);
      exit(2);
    }
    command = argv[i++];
    if (i < argc) arg0 = argv[i++];
  } else if (!sflag && i < argc) {
    script = argv[i++];
    arg0 = (char *)script;
  }
  pos_set(argc - i, argv + i);
  arg0 = xstrdup(arg0);

  iflag = iforce || (!command && !script && isatty(0) && isatty(2));
  optval[OPT_s] = (char)(!command && !script);
  if (script) progname = arg0;

  /* standard variables */
  var_set("IFS", " \t\n", 0);
  var_set("OPTIND", "1", 0);
  if (!var_get("PS4")) var_set("PS4", "+ ", 0);
  char num[24];
  snprintf(num, sizeof(num), "%ld", (long)getppid());
  var_set("PPID", num, 0);
  init_pwd();

  signals_init(iflag);
  /* job control is on by default in an interactive shell (POSIX), also one
   * made interactive with -i, when stdin and stderr are a terminal; +m
   * turns it off */
  if (!mgiven) optval[OPT_m] = (char)(iflag && isatty(0) && isatty(2));
  if (optval[OPT_m]) setjobctl(1);

  static struct source *src;
  if (script) {
    int fd = open(script, O_RDONLY);
    if (fd < 0) {
      fprintf(stderr, "%s: %s: %s\n", CPSH_NAME, script, strerror(errno));
      exit(errno == ENOENT ? 127 : 126);
    }
    src = src_fd(move_fd_high(fd), 0);
  } else if (!command) {
    src = src_fd(0, iflag);
  }

  static stackmark base;
  base = stmark();
  struct jmploc jl;
  if (setjmp(jl.buf)) {
    int interrupted = exception == EX_INT;
    reset_after_error();
    strelease(base);
    if (!iflag || !rootshell || command) shell_exit(interrupted ? 130 : 2);
    exitstatus = interrupted ? 130 : 2;
    if (src) src_reset(src);
  } else {
    handler = &jl;
    if (iflag) {
      history_init();
      source_env_file();
    }
  }
  handler = &jl;

  if (command) evalstring(command, EV_EXIT);
  evalsource(src, 1);
  if (iflag) fputc('\n', stderr);
  shell_exit(exitstatus);
}
