#ifndef VARS_H
#define VARS_H

#include "common.h"

#define V_EXPORT 0x1
#define V_READONLY 0x2
#define V_LINENO 0x4 /* LINENO: the value is computed when read */

struct var {
  struct var *next;
  char *name;
  char *val; /* NULL when declared (export/readonly) but unset */
  int flags;
  unsigned hash;
};

void vars_init(char **envp);
struct var *var_lookup(const char *name);
const char *var_get(const char *name);
/* Set a variable; flags are OR'ed into the existing ones. Returns -1 (after
 * printing a diagnostic) if the variable is readonly. */
int var_set(const char *name, const char *val, int flags);
int var_set_n(const char *name, size_t namelen, const char *val, int flags);
int var_set_assign(const char *assign, int flags); /* "NAME=value" */
int var_unset(const char *name);
char **var_environ(void); /* cached environment for exec */
/* Print variables having all of `flags` set, prefixed (e.g. "export "). */
void var_print(int flags, const char *prefix);

int valid_name(const char *s);
size_t name_len(const char *s); /* length of the leading valid name */

/* Scopes: function locals and temporary `VAR=x cmd` assignments. */
void var_scope_push(void);
void var_scope_pop(void);
int var_scope_depth(void);
int var_local(const char *name); /* save var in current scope; -1 on error */

/* Positional parameters ($1...). */
struct posparams {
  int argc;
  char **argv; /* malloc'd strings, argv[argc] == NULL */
};
extern struct posparams pos;
void pos_set(int argc, char **argv); /* copies */
void pos_free(struct posparams *p);

#endif
