#include "expand.h"

#include <fnmatch.h>
#include <glob.h>
#include <pwd.h>

#include "exec.h"
#include "parser.h"
#include "shell.h"
#include "vars.h"

int cmdsub_status = -1;

/* Character attributes in the expansion buffer. */
enum {
  A_LIT,   /* unquoted literal: globbed, not split */
  A_EXP,   /* unquoted expansion result: split and globbed */
  A_QUOT,  /* quoted: neither split nor globbed */
  A_BREAK, /* field boundary between "$@" elements */
  A_QEMPTY /* marks an empty quoted string ("" keeps an empty field) */
};

struct xc {
  char c, a;
};

struct xstate {
  struct xc *v;
  size_t len, cap;
  int heredoc;
  int saw_at; /* expanded "$@" inside the current double quotes */
};

#define F_ASSIGN 1 /* tilde expansion after ':' in assignments */
#define F_BRACE 2  /* word of a ${...} inside double quotes: "..." still quotes */

static void xinit(struct xstate *xs) { memset(xs, 0, sizeof(*xs)); }

static void xput(struct xstate *xs, char c, char a) {
  if (xs->len == xs->cap) {
    size_t ncap = xs->cap ? xs->cap * 2 : 64;
    xs->v = stgrow(xs->v, xs->cap * sizeof(struct xc), ncap * sizeof(struct xc));
    xs->cap = ncap;
  }
  xs->v[xs->len].c = c;
  xs->v[xs->len].a = a;
  xs->len++;
}

static void xputs(struct xstate *xs, const char *s, size_t n, char a) {
  for (size_t i = 0; i < n; i++) xput(xs, s[i], a);
}

/* Quote-removed string of the buffer. */
static char *xstring(struct xstate *xs, size_t from) {
  char *s = stalloc(xs->len - from + 1);
  size_t k = 0;
  for (size_t i = from; i < xs->len; i++) {
    if (xs->v[i].a == A_QEMPTY) continue;
    s[k++] = xs->v[i].a == A_BREAK ? ' ' : xs->v[i].c;
  }
  s[k] = '\0';
  return s;
}

/* fnmatch()/glob() pattern of the buffer: quoted characters are escaped. */
static char *xpattern(struct xstate *xs, size_t from) {
  char *s = stalloc(2 * (xs->len - from) + 1);
  size_t k = 0;
  for (size_t i = from; i < xs->len; i++) {
    char c = xs->v[i].c, a = xs->v[i].a;
    if (a == A_QEMPTY) continue;
    if (a == A_BREAK) c = ' ';
    if (a == A_QUOT && strchr("*?[]\\", c)) s[k++] = '\\';
    s[k++] = c;
  }
  s[k] = '\0';
  return s;
}

static void expand_part(struct xstate *xs, const char *p, const char *end,
                        int dq, int flags);

/* ---- tilde ---- */

static const char *expand_tilde(struct xstate *xs, const char *p,
                                const char *end, int flags) {
  const char *q = p + 1;
  while (q < end && *q != '/' && !((flags & F_ASSIGN) && *q == ':')) {
    if (strchr("'\"\\$`", *q)) return NULL; /* quoted: no expansion */
    q++;
  }
  const char *dir = NULL;
  if (q == p + 1) {
    dir = var_get("HOME");
    if (!dir) {
      struct passwd *pw = getpwuid(getuid());
      if (pw) dir = pw->pw_dir;
    }
  } else {
    char *user = ststrndup(p + 1, (size_t)(q - p - 1));
    struct passwd *pw = getpwnam(user);
    if (pw) dir = pw->pw_dir;
  }
  if (!dir) return NULL;
  xputs(xs, dir, strlen(dir), A_QUOT);
  if (!*dir && q == end) xput(xs, 0, A_QEMPTY);
  return q;
}

/* ---- command substitution ---- */

static void emit_cmdsub(struct xstate *xs, const char *cmd, size_t n, int dq) {
  size_t len;
  char *out = cmdsub_run(ststrndup(cmd, n), &len);
  while (len > 0 && out[len - 1] == '\n') len--;
  xputs(xs, out, len, dq ? A_QUOT : A_EXP);
  free(out);
}

static void emit_backquote(struct xstate *xs, const char *p, const char *end,
                           int dq) {
  strbuf sb;
  sb_init(&sb);
  for (; p < end; p++) {
    if (*p == '\\' && p + 1 < end &&
        (p[1] == '$' || p[1] == '`' || p[1] == '\\' || (dq && p[1] == '"')))
      p++;
    sb_putc(&sb, *p);
  }
  char *cmd = ststrndup(sb.s ? sb.s : "", sb.len);
  sb_free(&sb);
  emit_cmdsub(xs, cmd, strlen(cmd), dq);
}

/* ---- parameters ---- */

static int ifs_first(void) {
  const char *ifs = var_get("IFS");
  if (!ifs) return ' ';
  return *ifs ? (unsigned char)*ifs : -1;
}

/* Value of a scalar parameter, or NULL if unset. `buf` holds numbers. */
static const char *param_value(const char *name, size_t n, char *buf) {
  if (n == 1 || isdigit((unsigned char)name[0])) {
    switch (name[0]) {
      case '?':
        snprintf(buf, 32, "%d", exitstatus);
        return buf;
      case '$':
        snprintf(buf, 32, "%ld", (long)rootpid);
        return buf;
      case '!':
        if (!backgndpid) return NULL;
        snprintf(buf, 32, "%ld", (long)backgndpid);
        return buf;
      case '#':
        snprintf(buf, 32, "%d", pos.argc);
        return buf;
      case '-':
        options_string(buf);
        return buf;
    }
    if (isdigit((unsigned char)name[0])) {
      long i = 0;
      for (size_t k = 0; k < n; k++) i = i * 10 + (name[k] - '0');
      if (i == 0) return arg0;
      return i <= pos.argc ? pos.argv[i - 1] : NULL;
    }
  }
  char *nm = ststrndup(name, n);
  return var_get(nm);
}

/* "$*" / "$@" joined into one string. */
static char *join_params(void) {
  strbuf sb;
  sb_init(&sb);
  int sep = ifs_first();
  for (int i = 0; i < pos.argc; i++) {
    if (i && sep >= 0) sb_putc(&sb, (char)sep);
    sb_puts(&sb, pos.argv[i]);
  }
  char *s = ststrndup(sb.s ? sb.s : "", sb.len);
  sb_free(&sb);
  return s;
}

static void emit_params(struct xstate *xs, char which, int dq) {
  if (dq && which == '@') xs->saw_at = 1;
  int sep = ifs_first();
  for (int i = 0; i < pos.argc; i++) {
    if (i) {
      if (dq && which == '*') {
        if (sep >= 0) xput(xs, (char)sep, A_QUOT);
      } else {
        xput(xs, 0, A_BREAK);
      }
    }
    const char *v = pos.argv[i];
    xputs(xs, v, strlen(v), dq ? A_QUOT : A_EXP);
    if (dq && !*v) xput(xs, 0, A_QEMPTY);
  }
}

static size_t char_count(const char *s) {
  size_t n = 0;
  mblen(NULL, 0);
  while (*s) {
    int l = mblen(s, MB_CUR_MAX);
    s += l > 0 ? l : 1;
    n++;
  }
  return n;
}

static const char *remove_pattern(const char *val, const char *pat, int suffix,
                                  int longest) {
  size_t len = strlen(val);
  char *tmp = ststrdup(val);
  if (!suffix) {
    for (size_t k = 0; k <= len; k++) {
      size_t i = longest ? len - k : k;
      char save = tmp[i];
      tmp[i] = '\0';
      int m = fnmatch(pat, tmp, 0) == 0;
      tmp[i] = save;
      if (m) return val + i;
    }
  } else {
    for (size_t k = 0; k <= len; k++) {
      size_t i = longest ? k : len - k;
      if (fnmatch(pat, val + i, 0) == 0) return ststrndup(val, i);
    }
  }
  return val;
}

/* ${...}: p points just after "${", end at the closing '}'. */
static void expand_brace(struct xstate *xs, const char *p, const char *end,
                         int dq) {
  char buf[64];
  int length = 0;
  if (*p == '#' && p + 1 < end) {
    const char *q = p + 1;
    size_t n = isdigit((unsigned char)*q) ? strspn(q, "0123456789")
               : strchr("@*#?-$!", *q)    ? 1
                                          : name_len(q);
    if (n && q + n == end) {
      length = 1;
      p = q;
    }
  }
  const char *name = p;
  size_t n;
  if (isdigit((unsigned char)*p))
    n = strspn(p, "0123456789");
  else if (*p && strchr("@*#?-$!", *p))
    n = 1;
  else
    n = name_len(p);
  if (n == 0) sh_error("${%.*s}: bad substitution", (int)(end - p), p);
  const char *op = name + n;
  int colon = 0;
  char opc = 0;
  int twice = 0;
  if (op < end) {
    if (*op == ':' && op + 1 < end && strchr("-=?+", op[1])) {
      colon = 1;
      op++;
    }
    opc = *op;
    if (!strchr("-=?+%#", opc))
      sh_error("${%.*s}: bad substitution", (int)(end - name), name);
    op++;
    if ((opc == '%' || opc == '#') && op < end && *op == opc) {
      twice = 1;
      op++;
    }
  }
  if (length && opc)
    sh_error("${#%.*s}: bad substitution", (int)(end - name), name);

  int is_at = n == 1 && (*name == '@' || *name == '*');
  const char *val;
  int set;
  if (is_at) {
    set = pos.argc > 0;
    val = set ? join_params() : "";
  } else {
    val = param_value(name, n, buf);
    set = val != NULL;
  }
  if (!set && uflag && !is_at && opc != '-' && opc != '=' && opc != '?' &&
      opc != '+')
    sh_error("%.*s: parameter not set", (int)n, name);
  if (!val) val = "";
  int use_word = colon ? (!set || !*val) : !set;
  char attr = dq ? A_QUOT : A_EXP;

  if (length) {
    snprintf(buf, sizeof(buf), "%lu",
             (unsigned long)(is_at ? (size_t)pos.argc : char_count(val)));
    xputs(xs, buf, strlen(buf), attr);
    return;
  }
  switch (opc) {
    case 0:
      if (is_at)
        emit_params(xs, *name, dq);
      else
        xputs(xs, val, strlen(val), attr);
      return;
    case '-':
      if (use_word)
        expand_part(xs, op, end, dq, dq ? F_BRACE : 0);
      else if (is_at)
        emit_params(xs, *name, dq);
      else
        xputs(xs, val, strlen(val), attr);
      return;
    case '+':
      if (!use_word) expand_part(xs, op, end, dq, dq ? F_BRACE : 0);
      return;
    case '=':
      if (use_word) {
        struct xstate sub;
        xinit(&sub);
        expand_part(&sub, op, end, dq, dq ? F_BRACE : 0);
        val = xstring(&sub, 0);
        if (is_at || isdigit((unsigned char)*name) || (n == 1 && !name_len(name)))
          sh_error("%.*s: cannot assign in this way", (int)n, name);
        if (var_set_n(name, n, val, 0) < 0) raise_exception(EX_ERROR);
      }
      if (is_at)
        emit_params(xs, *name, dq);
      else
        xputs(xs, val, strlen(val), attr);
      return;
    case '?':
      if (use_word) {
        const char *msg = "parameter null or not set";
        if (op < end) {
          struct xstate sub;
          xinit(&sub);
          expand_part(&sub, op, end, dq, dq ? F_BRACE : 0);
          msg = xstring(&sub, 0);
        }
        sh_error("%.*s: %s", (int)n, name, msg);
      }
      if (is_at)
        emit_params(xs, *name, dq);
      else
        xputs(xs, val, strlen(val), attr);
      return;
    default: { /* % %% # ## */
      struct xstate sub;
      xinit(&sub);
      expand_part(&sub, op, end, 0, 0);
      const char *pat = xpattern(&sub, 0);
      const char *r = remove_pattern(val, pat, opc == '%', twice);
      xputs(xs, r, strlen(r), attr);
      return;
    }
  }
}

/* Expand a '$' construct at p; returns the position after it. */
static const char *expand_dollar(struct xstate *xs, const char *p,
                                 const char *end, int dq) {
  char buf[64];
  const char *q;
  char c = p + 1 < end ? p[1] : '\0';
  if (c == '{') {
    q = scan_dollar(p);
    if (q > end || q[-1] != '}') sh_error("bad substitution");
    expand_brace(xs, p + 2, q - 1, dq);
    return q;
  }
  if (c == '(') {
    q = scan_dollar(p);
    if (q > end) q = end;
    if (p[2] == '(' && q - p >= 5 && q[-1] == ')' && q[-2] == ')') {
      struct xstate sub;
      xinit(&sub);
      expand_part(&sub, p + 3, q - 2, 1, 0);
      snprintf(buf, sizeof(buf), "%ld", arith_eval(xstring(&sub, 0)));
      xputs(xs, buf, strlen(buf), dq ? A_QUOT : A_EXP);
      return q;
    }
    emit_cmdsub(xs, p + 2, (size_t)(q - 1 - (p + 2)), dq);
    return q;
  }
  if (c == '@' || c == '*') {
    emit_params(xs, c, dq);
    return p + 2;
  }
  size_t n = 0;
  if (c && (isdigit((unsigned char)c) || strchr("#?-$!", c)))
    n = 1;
  else
    n = name_len(p + 1);
  if (n == 0 || p + 1 + n > end) {
    xput(xs, '$', dq ? A_QUOT : A_LIT);
    return p + 1;
  }
  const char *val = param_value(p + 1, n, buf);
  if (!val && uflag)
    sh_error("%.*s: parameter not set", (int)n, p + 1);
  if (val) xputs(xs, val, strlen(val), dq ? A_QUOT : A_EXP);
  return p + 1 + n;
}

static void expand_part(struct xstate *xs, const char *p, const char *end,
                        int dq, int flags) {
  const char *start = p;
  while (p < end) {
    char c = *p;
    switch (c) {
      case '\'':
        if (dq || xs->heredoc) break;
        {
          const char *q = scan_squote(p + 1);
          if (q > end) q = end;
          size_t n = (size_t)(q - p - 2);
          if (q[-1] != '\'' || q == p + 1) n = (size_t)(q - p - 1);
          xputs(xs, p + 1, n, A_QUOT);
          if (n == 0) xput(xs, 0, A_QEMPTY);
          p = q;
        }
        continue;
      case '"':
        if ((dq && !(flags & F_BRACE)) || xs->heredoc) break;
        {
          const char *q = scan_dquote(p + 1);
          if (q > end) q = end;
          const char *inner_end = (q > p + 1 && q[-1] == '"') ? q - 1 : q;
          size_t before = xs->len;
          int saved_at = xs->saw_at;
          xs->saw_at = 0;
          expand_part(xs, p + 1, inner_end, 1, 0);
          if (xs->len == before && !xs->saw_at) xput(xs, 0, A_QEMPTY);
          xs->saw_at = saved_at;
          p = q;
        }
        continue;
      case '\\':
        if (p + 1 >= end) break;
        if (xs->heredoc) {
          if (strchr("$`\\", p[1])) {
            xput(xs, p[1], A_QUOT);
            p += 2;
            continue;
          }
          break;
        }
        if (dq) {
          if (strchr("$`\"\\", p[1])) {
            xput(xs, p[1], A_QUOT);
            p += 2;
            continue;
          }
          break;
        }
        xput(xs, p[1], A_QUOT);
        p += 2;
        continue;
      case '$':
        p = expand_dollar(xs, p, end, dq);
        continue;
      case '`': {
        const char *q = scan_backquote(p + 1);
        if (q > end) q = end;
        emit_backquote(xs, p + 1, q[-1] == '`' && q > p + 1 ? q - 1 : q, dq);
        p = q;
        continue;
      }
      case '~':
        if (!dq && !xs->heredoc &&
            (p == start || ((flags & F_ASSIGN) && p[-1] == ':'))) {
          const char *q = expand_tilde(xs, p, end, flags);
          if (q) {
            p = q;
            continue;
          }
        }
        break;
    }
    xput(xs, c, dq || xs->heredoc ? A_QUOT : A_LIT);
    p++;
  }
}

/* ---- field splitting and pathname expansion ---- */

void arglist_init(struct arglist *al) {
  al->n = 0;
  al->cap = 8;
  al->v = stalloc((size_t)al->cap * sizeof(char *));
  al->v[0] = NULL;
}

void arglist_add(struct arglist *al, char *s) {
  if (al->n + 1 >= al->cap) {
    int ncap = al->cap * 2;
    al->v = stgrow(al->v, (size_t)al->cap * sizeof(char *),
                   (size_t)ncap * sizeof(char *));
    al->cap = ncap;
  }
  al->v[al->n++] = s;
  al->v[al->n] = NULL;
}

static void add_field(struct xstate *xs, size_t from, size_t to,
                      struct arglist *out) {
  int globbable = 0;
  if (!fflag)
    for (size_t i = from; i < to; i++)
      if ((xs->v[i].a == A_LIT || xs->v[i].a == A_EXP) &&
          strchr("*?[", xs->v[i].c)) {
        globbable = 1;
        break;
      }
  struct xstate f = {xs->v + from, to - from, to - from, 0, 0};
  if (globbable) {
    glob_t g;
    char *pat = xpattern(&f, 0);
    if (glob(pat, 0, NULL, &g) == 0) {
      for (size_t i = 0; i < g.gl_pathc; i++)
        arglist_add(out, ststrdup(g.gl_pathv[i]));
      globfree(&g);
      return;
    }
  }
  arglist_add(out, xstring(&f, 0));
}

static void split_fields(struct xstate *xs, struct arglist *out) {
  const char *ifs = var_get("IFS");
  if (!ifs) ifs = " \t\n";
  size_t start = 0, i = 0;
  int started = 0;
  while (i < xs->len) {
    char c = xs->v[i].c, a = xs->v[i].a;
    if (a == A_BREAK) {
      if (started) add_field(xs, start, i, out);
      started = 0;
      start = ++i;
      continue;
    }
    if (a == A_EXP && c && strchr(ifs, c)) {
      int ws = c == ' ' || c == '\t' || c == '\n';
      if (started || !ws) add_field(xs, start, i, out);
      i++;
      /* swallow IFS whitespace, and at most one non-whitespace delimiter
       * that follows it when we were in whitespace */
      while (i < xs->len && xs->v[i].a == A_EXP && xs->v[i].c &&
             strchr(" \t\n", xs->v[i].c) && strchr(ifs, xs->v[i].c))
        i++;
      if (ws && i < xs->len && xs->v[i].a == A_EXP && xs->v[i].c &&
          strchr(ifs, xs->v[i].c) && !strchr(" \t\n", xs->v[i].c)) {
        if (!started) add_field(xs, i, i, out); /* empty field */
        i++;
        while (i < xs->len && xs->v[i].a == A_EXP && xs->v[i].c &&
               strchr(" \t\n", xs->v[i].c) && strchr(ifs, xs->v[i].c))
          i++;
      }
      started = 0;
      start = i;
      continue;
    }
    started = 1;
    i++;
  }
  if (started) add_field(xs, start, xs->len, out);
}

/* Words with none of these characters expand to themselves. */
static int plain_word(const char *w) {
  for (; *w; w++)
    if (strchr("$`'\"\\~*?[", *w)) return 0;
  return 1;
}

void expand_fields(const char *word, struct arglist *out) {
  if (plain_word(word)) {
    arglist_add(out, (char *)word);
    return;
  }
  struct xstate xs;
  xinit(&xs);
  expand_part(&xs, word, word + strlen(word), 0, 0);
  split_fields(&xs, out);
}

char *expand_str(const char *word, int assignment) {
  if (plain_word(word)) return (char *)word;
  struct xstate xs;
  xinit(&xs);
  expand_part(&xs, word, word + strlen(word), 0, assignment ? F_ASSIGN : 0);
  return xstring(&xs, 0);
}

char *expand_pattern(const char *word) {
  struct xstate xs;
  xinit(&xs);
  expand_part(&xs, word, word + strlen(word), 0, 0);
  return xpattern(&xs, 0);
}

char *expand_heredoc(const char *body) {
  struct xstate xs;
  xinit(&xs);
  xs.heredoc = 1;
  expand_part(&xs, body, body + strlen(body), 0, 0);
  return xstring(&xs, 0);
}

char *expand_prompt(const char *s) {
  struct xstate xs;
  xinit(&xs);
  expand_part(&xs, s, s + strlen(s), 1, 0);
  return xstring(&xs, 0);
}
