#ifndef BUILTINS_H
#define BUILTINS_H

struct builtin {
  const char *name;
  int (*fn)(int argc, char **argv);
  int special; /* POSIX special built-in */
};

const struct builtin *find_builtin(const char *name);
const struct builtin *builtin_table(void); /* terminated by a NULL name */

int exec_builtin(int argc, char **argv);
int test_builtin(int argc, char **argv);
int printf_builtin(int argc, char **argv);

#endif
