#ifndef SHELL_H
#define SHELL_H

#include "common.h"

/* ---- shell options (set -o / single letters) ---- */
enum {
  OPT_a, /* allexport */
  OPT_b, /* notify */
  OPT_C, /* noclobber */
  OPT_e, /* errexit */
  OPT_f, /* noglob */
  OPT_h, /* hashall */
  OPT_i, /* interactive */
  OPT_m, /* monitor */
  OPT_n, /* noexec */
  OPT_s, /* stdin */
  OPT_u, /* nounset */
  OPT_v, /* verbose */
  OPT_x, /* xtrace */
  OPT_ignoreeof,
  OPT_nolog,
  OPT_pipefail,
  OPT_vi,
  NOPTS
};

struct optdef {
  char letter; /* 0 for long-only options */
  const char *name;
};

extern char optval[NOPTS];
extern const struct optdef optdefs[NOPTS];

#define aflag optval[OPT_a]
#define Cflag optval[OPT_C]
#define eflag optval[OPT_e]
#define fflag optval[OPT_f]
#define iflag optval[OPT_i]
#define nflag optval[OPT_n]
#define uflag optval[OPT_u]
#define vflag optval[OPT_v]
#define xflag optval[OPT_x]

int setoption_letter(int letter, int on); /* -1 if unknown */
int setoption_name(const char *name, int on);
void options_string(char *buf); /* value of $- */

/* ---- global state ---- */
extern int exitstatus;     /* $? */
extern pid_t rootpid;      /* $$ */
extern pid_t backgndpid;   /* $! (0 if none) */
extern int rootshell;      /* 0 inside forked subshells */
extern const char *shell_exe; /* path used to run scripts lacking #! */
extern char *arg0;         /* $0 */

NORETURN void shell_exit(int status);

#endif
