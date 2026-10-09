#ifndef ALIAS_H
#define ALIAS_H

struct alias {
  struct alias *next;
  char *name;
  char *value;
  int active; /* >0 while its text is being read (prevents recursion) */
};

struct alias *alias_lookup(const char *name);
void alias_set(const char *name, const char *value);
int alias_unset(const char *name);
void alias_unset_all(void);
void alias_print(const struct alias *a);
void alias_print_all(void);

#endif
