#include "lexer.h"

static const char *const tok_names[] = {
    "end of file", "newline", "word", "io number", ";", "&",  "&&",
    "||",          "|",       "(",    ")",         ";;", "<",  ">",
    "<<",          "<<-",     ">>",   "<&",        ">&", "<>", ">|"};

const char *tok_str(int tok) { return tok_names[tok]; }

void lex_init(struct lexer *lx, struct source *src, arena *a) {
  memset(lx, 0, sizeof(*lx));
  lx->src = src;
  lx->a = a;
  lx->hd_tail = &lx->hd_head;
}

void lex_done(struct lexer *lx) { (void)lx; }

void lex_add_heredoc(struct lexer *lx, struct redir *r) {
  r->hdnext = NULL;
  *lx->hd_tail = r;
  lx->hd_tail = &r->hdnext;
}

static NORETURN void lex_error(struct lexer *lx, const char *msg) {
  sh_error("syntax error at line %d: %s", lx->src->lineno, msg);
}

static int rawc(struct lexer *lx) { return src_getc(lx->src); }
static void ungetc_(struct lexer *lx, int c) {
  if (c != PEOF) src_ungetc(lx->src, c);
}

/* getc that drops backslash-newline line continuations. */
static int getc_nl(struct lexer *lx) {
  for (;;) {
    int c = rawc(lx);
    if (c != '\\') return c;
    int n = rawc(lx);
    if (n == '\n') continue;
    ungetc_(lx, n);
    return '\\';
  }
}

/* The word buffer lives on the scratch stack, so a syntax error that
 * unwinds with longjmp cannot leak it. */
static void put(struct lexer *lx, int c) {
  if (lx->wlen + 2 > lx->wcap) {
    size_t n = lx->wcap ? lx->wcap * 2 : 128;
    lx->wbuf = stgrow(lx->wbuf, lx->wcap, n);
    lx->wcap = n;
  }
  lx->wbuf[lx->wlen++] = (char)c;
  lx->wbuf[lx->wlen] = '\0';
}

static void lex_dollar(struct lexer *lx, int indq);
static void lex_backquote(struct lexer *lx);

/* Copy an escaped character after a backslash that was already stored. */
static void lex_escape(struct lexer *lx) {
  int c = rawc(lx);
  if (c == PEOF) return;
  put(lx, c);
}

static void lex_squote(struct lexer *lx) {
  for (;;) {
    int c = rawc(lx);
    if (c == PEOF) lex_error(lx, "unterminated quoted string");
    put(lx, c);
    if (c == '\'') return;
  }
}

static void lex_dquote(struct lexer *lx) {
  for (;;) {
    int c = getc_nl(lx);
    if (c == PEOF) lex_error(lx, "unterminated quoted string");
    put(lx, c);
    if (c == '"') return;
    if (c == '\\')
      lex_escape(lx);
    else if (c == '$')
      lex_dollar(lx, 1);
    else if (c == '`')
      lex_backquote(lx);
  }
}

static void lex_backquote(struct lexer *lx) {
  for (;;) {
    int c = getc_nl(lx);
    if (c == PEOF) lex_error(lx, "unterminated `...`");
    put(lx, c);
    if (c == '`') return;
    if (c == '\\') lex_escape(lx);
  }
}

/* Body of $( ... ) after "$(" has been stored. Tracks nested parentheses
 * and case/esac so `case x in a) ...` patterns do not end the substitution
 * early. */
static void lex_cmdsub(struct lexer *lx) {
  int depth = 0, casedepth = 0, wordstart = 1;
  for (;;) {
    int c = getc_nl(lx);
    if (c == PEOF) lex_error(lx, "unterminated $(...)");
    put(lx, c);
    if (wordstart && isalpha(c)) {
      /* collect a plain word to spot the case/esac keywords */
      char w[5];
      size_t len = 0;
      int n;
      w[len++] = (char)c;
      while ((n = getc_nl(lx)) != PEOF && (isalnum(n) || n == '_')) {
        put(lx, n);
        if (len < 5) w[len] = (char)n;
        len++;
      }
      ungetc_(lx, n);
      if (len == 4 && (n == PEOF || strchr(" \t\n;&|()", n))) {
        if (memcmp(w, "case", 4) == 0) casedepth++;
        if (memcmp(w, "esac", 4) == 0 && casedepth > 0) casedepth--;
      }
      wordstart = 0;
      continue;
    }
    switch (c) {
      case '\\':
        lex_escape(lx);
        break;
      case '\'':
        lex_squote(lx);
        break;
      case '"':
        lex_dquote(lx);
        break;
      case '`':
        lex_backquote(lx);
        break;
      case '$':
        lex_dollar(lx, 0);
        break;
      case '#':
        if (wordstart) {
          while ((c = rawc(lx)) != PEOF && c != '\n') put(lx, c);
          if (c == '\n') put(lx, c);
        }
        break;
      case '(':
        depth++;
        break;
      case ')':
        if (depth == 0) {
          if (casedepth == 0) return;
        } else {
          depth--;
        }
        break;
    }
    wordstart = strchr(" \t\n;&|()", c) != NULL;
  }
}

/* Called after '$' has been stored. */
static void lex_dollar(struct lexer *lx, int indq) {
  int c = getc_nl(lx);
  if (c == '{') {
    put(lx, c);
    for (;;) {
      c = getc_nl(lx);
      if (c == PEOF) lex_error(lx, "unterminated ${...}");
      put(lx, c);
      if (c == '}') return;
      if (c == '\\')
        lex_escape(lx);
      else if (c == '\'' && !indq)
        lex_squote(lx);
      else if (c == '"')
        lex_dquote(lx);
      else if (c == '`')
        lex_backquote(lx);
      else if (c == '$')
        lex_dollar(lx, indq);
    }
  } else if (c == '(') {
    put(lx, c);
    int n = getc_nl(lx);
    if (n == '(') {
      /* $(( arithmetic )) */
      put(lx, n);
      int depth = 0;
      for (;;) {
        c = getc_nl(lx);
        if (c == PEOF) lex_error(lx, "unterminated $((...))");
        put(lx, c);
        if (c == '(') {
          depth++;
        } else if (c == ')') {
          if (depth > 0) {
            depth--;
            continue;
          }
          c = getc_nl(lx);
          if (c == ')') {
            put(lx, c);
            return;
          }
          ungetc_(lx, c);
        } else if (c == '\\') {
          lex_escape(lx);
        } else if (c == '\'') {
          lex_squote(lx);
        } else if (c == '"') {
          lex_dquote(lx);
        } else if (c == '`') {
          lex_backquote(lx);
        } else if (c == '$') {
          lex_dollar(lx, 1);
        }
      }
    }
    ungetc_(lx, n);
    lex_cmdsub(lx);
  } else {
    ungetc_(lx, c);
  }
}

/* Remove quoting from a here-document delimiter. */
static char *unquote_delim(const char *s, int *quoted) {
  strbuf sb;
  sb_init(&sb);
  *quoted = 0;
  for (; *s; s++) {
    if (*s == '\\' && s[1]) {
      *quoted = 1;
      sb_putc(&sb, *++s);
    } else if (*s == '\'' || *s == '"') {
      *quoted = 1;
    } else {
      sb_putc(&sb, *s);
    }
  }
  return sb_detach(&sb);
}

static void read_heredocs(struct lexer *lx) {
  struct redir *r = lx->hd_head;
  lx->hd_head = NULL;
  lx->hd_tail = &lx->hd_head;
  strbuf line, body;
  sb_init(&line);
  sb_init(&body);
  while (r) {
    struct redir *next = r->hdnext;
    int quoted;
    char *delim = unquote_delim(r->word, &quoted);
    r->hd_quoted = quoted;
    body.len = 0;
    if (body.s) body.s[0] = '\0';
    for (;;) {
      int c;
      line.len = 0;
      if (line.s) line.s[0] = '\0';
      int at_start = 1, eof = 0;
      for (;;) {
        c = rawc(lx);
        if (c == PEOF) {
          eof = 1;
          break;
        }
        if (c == '\t' && at_start && r->hd_strip) continue;
        at_start = 0;
        if (c == '\\' && !quoted) {
          int n = rawc(lx);
          if (n == '\n') {
            at_start = 0;
            continue;
          }
          sb_putc(&line, '\\');
          if (n == PEOF) {
            eof = 1;
            break;
          }
          c = n;
        }
        if (c == '\n') break;
        sb_putc(&line, (char)c);
      }
      if (strcmp(line.s ? line.s : "", delim) == 0) break;
      if (eof) {
        if (line.len) sb_putn(&body, line.s, line.len);
        if (line.len) sb_putc(&body, '\n');
        break;
      }
      sb_putn(&body, line.s ? line.s : "", line.len);
      sb_putc(&body, '\n');
    }
    r->heredoc = arena_strndup(lx->a, body.s ? body.s : "", body.len);
    free(delim);
    r = next;
  }
  sb_free(&line);
  sb_free(&body);
}

static int is_meta(int c) {
  /* NUL is an ordinary character: strchr() would match the terminator */
  return c == PEOF || (c != 0 && strchr(" \t\n;&|<>()", c) != NULL);
}

void lex_next(struct lexer *lx) {
  int c;
  lx->text = NULL;
  lx->quoted = 0;
  /* skip blanks and comments */
  for (;;) {
    c = getc_nl(lx);
    if (c == ' ' || c == '\t') continue;
    if (c == '#') {
      while ((c = rawc(lx)) != PEOF && c != '\n') {
      }
    }
    break;
  }
  lx->lineno = lx->src->lineno;
  switch (c) {
    case PEOF:
      lx->tok = T_EOF;
      return;
    case '\n':
      lx->tok = T_NL;
      if (lx->hd_head) read_heredocs(lx);
      return;
    case ';':
      c = getc_nl(lx);
      if (c == ';') {
        lx->tok = T_DSEMI;
        return;
      }
      ungetc_(lx, c);
      lx->tok = T_SEMI;
      return;
    case '&':
      c = getc_nl(lx);
      if (c == '&') {
        lx->tok = T_AND;
        return;
      }
      ungetc_(lx, c);
      lx->tok = T_AMP;
      return;
    case '|':
      c = getc_nl(lx);
      if (c == '|') {
        lx->tok = T_OR;
        return;
      }
      ungetc_(lx, c);
      lx->tok = T_PIPE;
      return;
    case '(':
      lx->tok = T_LPAREN;
      return;
    case ')':
      lx->tok = T_RPAREN;
      return;
    case '<':
      c = getc_nl(lx);
      if (c == '<') {
        c = getc_nl(lx);
        if (c == '-') {
          lx->tok = T_DLESSDASH;
          return;
        }
        ungetc_(lx, c);
        lx->tok = T_DLESS;
      } else if (c == '&') {
        lx->tok = T_LESSAND;
      } else if (c == '>') {
        lx->tok = T_LESSGREAT;
      } else {
        ungetc_(lx, c);
        lx->tok = T_LESS;
      }
      return;
    case '>':
      c = getc_nl(lx);
      if (c == '>') {
        lx->tok = T_DGREAT;
      } else if (c == '&') {
        lx->tok = T_GREATAND;
      } else if (c == '|') {
        lx->tok = T_CLOBBER;
      } else {
        ungetc_(lx, c);
        lx->tok = T_GREAT;
      }
      return;
  }

  /* a word */
  lx->wlen = 0;
  int quoted = 0;
  for (; !is_meta(c); c = getc_nl(lx)) {
    put(lx, c);
    switch (c) {
      case '\\':
        quoted = 1;
        lex_escape(lx);
        break;
      case '\'':
        quoted = 1;
        lex_squote(lx);
        break;
      case '"':
        quoted = 1;
        lex_dquote(lx);
        break;
      case '`':
        lex_backquote(lx);
        break;
      case '$':
        lex_dollar(lx, 0);
        break;
    }
  }
  if (!quoted && (c == '<' || c == '>') && is_number(lx->wbuf) &&
      lx->wlen < 5) {
    ungetc_(lx, c);
    lx->tok = T_IONUM;
    lx->ionum = atoi(lx->wbuf);
    return;
  }
  ungetc_(lx, c);
  lx->tok = T_WORD;
  lx->quoted = quoted;
  lx->text = arena_strndup(lx->a, lx->wbuf, lx->wlen);
}

/* ---- string scanners for the expander ---- */

const char *scan_squote(const char *p) {
  while (*p && *p != '\'') p++;
  return *p ? p + 1 : p;
}

const char *scan_backquote(const char *p) {
  while (*p && *p != '`') {
    if (*p == '\\' && p[1]) p++;
    p++;
  }
  return *p ? p + 1 : p;
}

static const char *scan_dollar_ctx(const char *p, int indq);

const char *scan_dquote(const char *p) {
  while (*p && *p != '"') {
    if (*p == '\\' && p[1])
      p += 2;
    else if (*p == '$')
      p = scan_dollar_ctx(p, 1);
    else if (*p == '`')
      p = scan_backquote(p + 1);
    else
      p++;
  }
  return *p ? p + 1 : p;
}

static const char *scan_cmdsub(const char *p) {
  int depth = 0, casedepth = 0, wordstart = 1;
  while (*p) {
    char c = *p;
    if (wordstart && (strncmp(p, "case", 4) == 0 || strncmp(p, "esac", 4) == 0) &&
        (!p[4] || strchr(" \t\n;&|()", p[4]))) {
      casedepth += c == 'c' ? 1 : -1;
      if (casedepth < 0) casedepth = 0;
      p += 4;
      wordstart = 0;
      continue;
    }
    switch (c) {
      case '\\':
        if (p[1]) p++;
        p++;
        break;
      case '\'':
        p = scan_squote(p + 1);
        break;
      case '"':
        p = scan_dquote(p + 1);
        break;
      case '`':
        p = scan_backquote(p + 1);
        break;
      case '$':
        p = scan_dollar_ctx(p, 0);
        break;
      case '#':
        if (wordstart) {
          while (*p && *p != '\n') p++;
        } else {
          p++;
        }
        break;
      case '(':
        depth++;
        p++;
        break;
      case ')':
        p++;
        if (depth == 0) {
          if (casedepth == 0) return p;
        } else {
          depth--;
        }
        break;
      default:
        p++;
    }
    wordstart = strchr(" \t\n;&|()", c) != NULL;
  }
  return p;
}

static const char *scan_dollar_ctx(const char *p, int indq) {
  p++; /* the '$' */
  if (*p == '{') {
    p++;
    while (*p && *p != '}') {
      if (*p == '\\' && p[1])
        p += 2;
      else if (*p == '\'' && !indq)
        p = scan_squote(p + 1);
      else if (*p == '"')
        p = scan_dquote(p + 1);
      else if (*p == '`')
        p = scan_backquote(p + 1);
      else if (*p == '$')
        p = scan_dollar_ctx(p, indq);
      else
        p++;
    }
    return *p ? p + 1 : p;
  }
  if (*p == '(' && p[1] == '(') {
    const char *q = p + 2;
    int depth = 0;
    while (*q) {
      if (*q == '(') {
        depth++;
        q++;
      } else if (*q == ')') {
        if (depth > 0) {
          depth--;
          q++;
        } else if (q[1] == ')') {
          return q + 2;
        } else {
          q++;
        }
      } else if (*q == '\\' && q[1]) {
        q += 2;
      } else if (*q == '\'') {
        q = scan_squote(q + 1);
      } else if (*q == '"') {
        q = scan_dquote(q + 1);
      } else if (*q == '`') {
        q = scan_backquote(q + 1);
      } else if (*q == '$') {
        q = scan_dollar_ctx(q, 1);
      } else {
        q++;
      }
    }
    return q;
  }
  if (*p == '(') return scan_cmdsub(p + 1);
  return p;
}

const char *scan_dollar(const char *p) { return scan_dollar_ctx(p, 0); }
