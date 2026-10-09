#include "vars.h"

#include "exec.h"
#include "shell.h"

#define NBUCKETS 256

static struct var *table[NBUCKETS];
static char **env_cache;
static int env_dirty = 1;

struct posparams pos;

static unsigned hash_name(const char *s, size_t n) {
  unsigned h = 2166136261u;
  for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
  return h;
}

size_t name_len(const char *s) {
  size_t n = 0;
  if (!(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
  while (isalnum((unsigned char)s[n]) || s[n] == '_') n++;
  return n;
}

int valid_name(const char *s) { return *s && s[name_len(s)] == '\0'; }

static struct var *lookup_n(const char *name, size_t n, unsigned h) {
  for (struct var *v = table[h % NBUCKETS]; v; v = v->next)
    if (v->hash == h && strncmp(v->name, name, n) == 0 && v->name[n] == '\0')
      return v;
  return NULL;
}

struct var *var_lookup(const char *name) {
  size_t n = strlen(name);
  return lookup_n(name, n, hash_name(name, n));
}

/* Bring LINENO's value up to date before it is read. */
static void refresh(struct var *v) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%d", cur_lineno);
  if (v->val && strcmp(v->val, buf) == 0) return;
  free(v->val);
  v->val = xstrdup(buf);
  if (v->flags & V_EXPORT) env_dirty = 1;
}

const char *var_get(const char *name) {
  struct var *v = var_lookup(name);
  if (v && (v->flags & V_LINENO)) refresh(v);
  return v ? v->val : NULL;
}

/* Hooks for variables the shell itself cares about. */
static void var_changed(const char *name) {
  if (strcmp(name, "PATH") == 0) hash_clear();
}

int var_set_n(const char *name, size_t n, const char *val, int flags) {
  unsigned h = hash_name(name, n);
  struct var *v = lookup_n(name, n, h);
  if (aflag && val) flags |= V_EXPORT;
  if (v) {
    if ((v->flags & V_READONLY) && val) {
      sh_warn("%s: is read only", v->name);
      return -1;
    }
    if (val) {
      char *nv = xstrdup(val);
      free(v->val);
      v->val = nv;
    }
    if ((v->flags | flags) & V_EXPORT) env_dirty = 1;
    v->flags |= flags;
  } else {
    v = xmalloc(sizeof(*v));
    v->name = xstrndup(name, n);
    v->val = val ? xstrdup(val) : NULL;
    v->flags = flags;
    v->hash = h;
    v->next = table[h % NBUCKETS];
    table[h % NBUCKETS] = v;
    if (flags & V_EXPORT) env_dirty = 1;
  }
  var_changed(v->name);
  return 0;
}

int var_set(const char *name, const char *val, int flags) {
  return var_set_n(name, strlen(name), val, flags);
}

int var_set_assign(const char *assign, int flags) {
  const char *eq = strchr(assign, '=');
  if (!eq) return var_set(assign, NULL, flags);
  return var_set_n(assign, (size_t)(eq - assign), eq + 1, flags);
}

int var_unset(const char *name) {
  size_t n = strlen(name);
  unsigned h = hash_name(name, n);
  struct var **pp = &table[h % NBUCKETS];
  for (; *pp; pp = &(*pp)->next) {
    struct var *v = *pp;
    if (v->hash != h || strcmp(v->name, name) != 0) continue;
    if (v->flags & V_READONLY) {
      sh_warn("%s: is read only", name);
      return -1;
    }
    if (v->flags & V_EXPORT) env_dirty = 1;
    *pp = v->next;
    var_changed(name);
    free(v->name);
    free(v->val);
    free(v);
    return 0;
  }
  return 0;
}

void vars_init(char **envp) {
  for (; envp && *envp; envp++) {
    const char *eq = strchr(*envp, '=');
    if (!eq || eq == *envp) continue;
    size_t n = (size_t)(eq - *envp);
    /* Only import names the shell can address. */
    if (name_len(*envp) != n) continue;
    var_set_n(*envp, n, eq + 1, V_EXPORT);
  }
  var_set("LINENO", NULL, V_LINENO);
}

char **var_environ(void) {
  struct var *ln = var_lookup("LINENO");
  if (ln && (ln->flags & V_LINENO) && (ln->flags & V_EXPORT)) refresh(ln);
  if (!env_dirty && env_cache) return env_cache;
  if (env_cache) {
    for (char **e = env_cache; *e; e++) free(*e);
    free(env_cache);
  }
  size_t n = 0, i = 0;
  for (int b = 0; b < NBUCKETS; b++)
    for (struct var *v = table[b]; v; v = v->next)
      if ((v->flags & V_EXPORT) && v->val) n++;
  env_cache = xmalloc((n + 1) * sizeof(char *));
  for (int b = 0; b < NBUCKETS; b++)
    for (struct var *v = table[b]; v; v = v->next) {
      if (!(v->flags & V_EXPORT) || !v->val) continue;
      size_t nl = strlen(v->name), vl = strlen(v->val);
      char *e = xmalloc(nl + vl + 2);
      memcpy(e, v->name, nl);
      e[nl] = '=';
      memcpy(e + nl + 1, v->val, vl + 1);
      env_cache[i++] = e;
    }
  env_cache[i] = NULL;
  env_dirty = 0;
  return env_cache;
}

static int cmp_var(const void *a, const void *b) {
  return strcmp((*(struct var *const *)a)->name, (*(struct var *const *)b)->name);
}

void var_print(int flags, const char *prefix) {
  size_t n = 0, cap = 64;
  struct var **list = xmalloc(cap * sizeof(*list));
  for (int b = 0; b < NBUCKETS; b++)
    for (struct var *v = table[b]; v; v = v->next) {
      if ((v->flags & flags) != flags) continue;
      if (v->flags & V_LINENO) refresh(v);
      if (!flags && !v->val) continue;
      if (n == cap) list = xrealloc(list, (cap *= 2) * sizeof(*list));
      list[n++] = v;
    }
  qsort(list, n, sizeof(*list), cmp_var);
  strbuf sb;
  sb_init(&sb);
  for (size_t i = 0; i < n; i++) {
    sb.len = 0;
    sb_puts(&sb, prefix);
    sb_puts(&sb, list[i]->name);
    if (list[i]->val) {
      sb_putc(&sb, '=');
      sh_quote(&sb, list[i]->val);
    }
    puts(sb.s);
  }
  sb_free(&sb);
  free(list);
}

/* ---- scopes ---- */

struct saved {
  struct saved *next;
  char *name;
  char *val;
  int flags;
  int existed;
};

struct scope {
  struct scope *prev;
  struct saved *saved;
};

static struct scope *scopes;
static int scope_depth;

void var_scope_push(void) {
  struct scope *s = xmalloc(sizeof(*s));
  s->prev = scopes;
  s->saved = NULL;
  scopes = s;
  scope_depth++;
}

int var_scope_depth(void) { return scope_depth; }

int var_local(const char *name) {
  if (!scopes) return 0;
  for (struct saved *sv = scopes->saved; sv; sv = sv->next)
    if (strcmp(sv->name, name) == 0) return 0;
  struct var *v = var_lookup(name);
  if (v && (v->flags & V_READONLY)) {
    sh_warn("%s: is read only", name);
    return -1;
  }
  struct saved *sv = xmalloc(sizeof(*sv));
  sv->name = xstrdup(name);
  sv->existed = v != NULL;
  sv->val = v && v->val ? xstrdup(v->val) : NULL;
  sv->flags = v ? v->flags : 0;
  sv->next = scopes->saved;
  scopes->saved = sv;
  return 0;
}

void var_scope_pop(void) {
  struct scope *s = scopes;
  if (!s) return;
  scopes = s->prev;
  scope_depth--;
  struct saved *sv = s->saved;
  while (sv) {
    struct saved *next = sv->next;
    struct var *v = var_lookup(sv->name);
    if (v) v->flags &= ~V_READONLY; /* restoring may undo a readonly */
    if (!sv->existed) {
      var_unset(sv->name);
    } else {
      if (v) {
        free(v->val);
        v->val = NULL;
        if (v->flags & V_EXPORT) env_dirty = 1;
        v->flags = 0;
      }
      var_set(sv->name, sv->val, 0);
      v = var_lookup(sv->name);
      if (v) {
        v->flags = sv->flags;
        if (v->flags & V_EXPORT) env_dirty = 1;
      }
    }
    free(sv->name);
    free(sv->val);
    free(sv);
    sv = next;
  }
  free(s);
}

/* ---- positional parameters ---- */

void pos_set(int argc, char **argv) {
  struct posparams np;
  np.argc = argc;
  np.argv = xmalloc((size_t)(argc + 1) * sizeof(char *));
  for (int i = 0; i < argc; i++) np.argv[i] = xstrdup(argv[i]);
  np.argv[argc] = NULL;
  pos_free(&pos);
  pos = np;
}

void pos_free(struct posparams *p) {
  if (p->argv) {
    for (int i = 0; i < p->argc; i++) free(p->argv[i]);
    free(p->argv);
  }
  p->argv = NULL;
  p->argc = 0;
}
