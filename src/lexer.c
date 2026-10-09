#include "lexer.h"

static const char *const tok_names[] = {
    "end of file", "newline", "word", "io number", ";", "&",  "&&",
    "||",          "|",       "(",    ")",         ";;", "<",  ">",
    "<<",          "<<-",     ">>",   "<&",        ">&", "<>", ">|",
    ";&"};

const char *tok_str(int tok) { return tok_names[tok]; }

void lex_init(struct lexer *lx, struct source *src, arena *a) {
  memset(lx, 0, sizeof(*lx));
  lx->src = src;
  lx->a = a;
  lx->hd_tail = &lx->hd_head;
}

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

static int lex_dollar(struct lexer *lx, int indq);
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

/* Body of $( ... ) after "$(" has been stored. The parser finds where it
 * ends, so case patterns, comments and here-documents inside it cannot end
 * it early; the text it read is recorded and kept as part of the word. */
static void lex_cmdsub(struct lexer *lx) {
  struct source *src = lx->src;
  int outermost = src->rec == NULL;
  if (outermost) {
    src->rec = xmalloc(sizeof(strbuf));
    sb_init(src->rec);
  }
  size_t from = src->rec->len;
  parse_cmdsub(src, lx->a);
  for (size_t i = from; i < src->rec->len; i++) put(lx, src->rec->s[i]);
  if (outermost) src_rec_end(src);
}

static void put_squoted(struct lexer *lx, int c) {
  if (c == '\'') { /* close the quotes, add \', reopen */
    put(lx, '\'');
    put(lx, '\\');
    put(lx, '\'');
  }
  put(lx, c);
}

static int hexval(int c) {
  if (isdigit(c)) return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* $'...' after "$'" has been read: store it as a single-quoted string with
 * its backslash escapes decoded. A decoded NUL ends the string. */
static void lex_dollar_squote(struct lexer *lx) {
  int nul = 0;
  put(lx, '\'');
  for (;;) {
    int c = rawc(lx);
    if (c == PEOF) lex_error(lx, "unterminated quoted string");
    if (c == '\'') break;
    if (c == '\\') {
      c = rawc(lx);
      if (c == PEOF) lex_error(lx, "unterminated quoted string");
      switch (c) {
        case 'a': c = '\a'; break;
        case 'b': c = '\b'; break;
        case 'e': c = 033; break;
        case 'f': c = '\f'; break;
        case 'n': c = '\n'; break;
        case 'r': c = '\r'; break;
        case 't': c = '\t'; break;
        case 'v': c = '\v'; break;
        case '\\': case '\'': case '"': break;
        case 'c': { /* control character */
          int n = rawc(lx);
          if (n == '\\') n = rawc(lx); /* \c\\ */
          if (n == PEOF) lex_error(lx, "unterminated quoted string");
          c = n == '?' ? 0177 : toupper(n) & 037;
          break;
        }
        case 'x': {
          int v = 0, k = 0, d;
          while (k < 2) {
            if ((d = hexval(c = rawc(lx))) < 0) {
              ungetc_(lx, c);
              break;
            }
            v = v * 16 + d;
            k++;
          }
          if (!k) { /* not an escape after all */
            if (!nul) {
              put(lx, '\\');
              put(lx, 'x');
            }
            continue;
          }
          c = v;
          break;
        }
        default:
          if (c >= '0' && c <= '7') {
            int v = c - '0', k = 1;
            while (k < 3 && (c = rawc(lx)) >= '0' && c <= '7') {
              v = v * 8 + c - '0';
              k++;
            }
            if (k < 3) ungetc_(lx, c);
            c = v & 0377;
          } else if (!nul) { /* unknown: keep the backslash */
            put(lx, '\\');
          }
      }
    }
    if (c == 0) nul = 1;
    if (!nul) put_squoted(lx, c);
  }
  put(lx, '\'');
}

/* Called after '$' has been stored. Returns 1 if it read a $'...'. */
static int lex_dollar(struct lexer *lx, int indq) {
  int c = getc_nl(lx);
  if (c == '\'' && !indq) {
    lx->wlen--; /* drop the '$' */
    lex_dollar_squote(lx);
    return 1;
  }
  if (c == '{') {
    put(lx, c);
    for (;;) {
      c = getc_nl(lx);
      if (c == PEOF) lex_error(lx, "unterminated ${...}");
      put(lx, c);
      if (c == '}') return 0;
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
            return 0;
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
  return 0;
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
      if (c == '&') {
        lx->tok = T_SEMIAND;
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
        if (lex_dollar(lx, 0)) quoted = 1;
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

/* p points just past "$(". Text from a parsed word is known to be well
 * formed; other text (prompts, here-document bodies) may not be, and then
 * the rest of the string is taken as the command. */
static const char *scan_cmdsub(const char *p) {
  struct source *volatile src = src_string(p);
  arena *volatile a = arena_new();
  struct jmploc jl, *saved = handler;
  const char *end;
  if (setjmp(jl.buf)) {
    end = p + strlen(p);
  } else {
    handler = &jl;
    parse_cmdsub(src, a);
    end = p + src->strpos;
  }
  handler = saved;
  arena_unref(a);
  src_free(src);
  return end;
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
