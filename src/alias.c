#include "alias.h"

#include "common.h"

static struct alias *aliases;

struct alias *alias_lookup(const char *name) {
  for (struct alias *a = aliases; a; a = a->next)
    if (strcmp(a->name, name) == 0) return a;
  return NULL;
}

void alias_set(const char *name, const char *value) {
  struct alias *a = alias_lookup(name);
  if (a) {
    free(a->value);
    a->value = xstrdup(value);
    return;
  }
  a = xcalloc(1, sizeof(*a));
  a->name = xstrdup(name);
  a->value = xstrdup(value);
  /* keep the list sorted for printing */
  struct alias **pp = &aliases;
  while (*pp && strcmp((*pp)->name, name) < 0) pp = &(*pp)->next;
  a->next = *pp;
  *pp = a;
}

static void alias_free(struct alias *a) {
  /* An alias being expanded is still referenced by the input; leak its
   * small record rather than free it from under the reader. */
  if (a->active) return;
  free(a->name);
  free(a->value);
  free(a);
}

int alias_unset(const char *name) {
  for (struct alias **pp = &aliases; *pp; pp = &(*pp)->next) {
    if (strcmp((*pp)->name, name) == 0) {
      struct alias *a = *pp;
      *pp = a->next;
      alias_free(a);
      return 0;
    }
  }
  return -1;
}

void alias_unset_all(void) {
  while (aliases) {
    struct alias *a = aliases;
    aliases = a->next;
    alias_free(a);
  }
}

void alias_print(const struct alias *a) {
  strbuf sb;
  sb_init(&sb);
  sb_puts(&sb, a->name);
  sb_putc(&sb, '=');
  sh_quote(&sb, a->value);
  puts(sb.s);
  sb_free(&sb);
}

void alias_print_all(void) {
  for (struct alias *a = aliases; a; a = a->next) alias_print(a);
}
