#ifndef BUILTINS_H
#define BUILTINS_H

#include "common.h"

struct builtin {
  const char *name;
  int (*fn)(int argc, char **argv);
  int special; /* POSIX special built-in */
};

const struct builtin *find_builtin(const char *name);
const struct builtin *builtin_table(void); /* terminated by a NULL name */

/* Errors in special built-ins abort a non-interactive shell (POSIX 2.8.1),
 * except when the built-in was run through `command`, which sets
 * special_soft: then they only make it fail with status 2. special_error()
 * reports such an error; special_fail() handles one already reported. */
extern int special_soft;
int special_error(const char *fmt, ...) PRINTFLIKE(1, 2);
int special_fail(void);

int exec_builtin(int argc, char **argv);
int test_builtin(int argc, char **argv);
int printf_builtin(int argc, char **argv);

#endif
