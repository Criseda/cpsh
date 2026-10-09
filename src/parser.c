#include "parser.h"

#include "alias.h"
#include "lexer.h"
#include "vars.h"

arena *parse_arena;

struct parser {
  struct lexer lx;
  int peeked;
  int alias_next; /* last alias ended in a blank: check the next word too */
};

static int peek(struct parser *P) {
  if (!P->peeked) {
    lex_next(&P->lx);
    P->peeked = 1;
  }
  return P->lx.tok;
}

static void consume(struct parser *P) { P->peeked = 0; }

static NORETURN void syntax_error(struct parser *P) {
  int t = peek(P);
  if (t == T_WORD)
    sh_error("syntax error at line %d: unexpected '%s'", P->lx.lineno,
             P->lx.text);
  if (t == T_EOF)
    sh_error("syntax error at line %d: unexpected end of file", P->lx.lineno);
  sh_error("syntax error at line %d: unexpected '%s'", P->lx.lineno,
           tok_str(t));
}

static int is_kw(struct parser *P, const char *kw) {
  return peek(P) == T_WORD && !P->lx.quoted && strcmp(P->lx.text, kw) == 0;
}

static void expect_kw(struct parser *P, const char *kw) {
  if (!is_kw(P, kw)) syntax_error(P);
  consume(P);
}

static void expect(struct parser *P, int tok) {
  if (peek(P) != tok) syntax_error(P);
  consume(P);
}

static void skip_newlines(struct parser *P) {
  while (peek(P) == T_NL) consume(P);
}

static int at_terminator(struct parser *P) {
  static const char *const ends[] = {"then", "else", "elif", "fi",  "do",
                                     "done", "esac", "}",    NULL};
  int t = peek(P);
  if (t == T_RPAREN || t == T_DSEMI || t == T_EOF) return 1;
  if (t != T_WORD || P->lx.quoted) return 0;
  for (int i = 0; ends[i]; i++)
    if (strcmp(P->lx.text, ends[i]) == 0) return 1;
  return 0;
}

static struct node *mknode(struct parser *P, int type) {
  struct node *n = arena_alloc(P->lx.a, sizeof(*n));
  memset(n, 0, sizeof(*n));
  n->type = type;
  n->lineno = P->lx.lineno;
  return n;
}

/* Append to an arena-backed array, doubling its capacity as needed. */
static void vpush(struct parser *P, void ***arr, int *n, int *cap, void *x) {
  if (*n + 1 >= *cap) {
    int ncap = *cap ? *cap * 2 : 8;
    void **na = arena_alloc(P->lx.a, (size_t)ncap * sizeof(void *));
    if (*n) memcpy(na, *arr, (size_t)*n * sizeof(void *));
    *arr = na;
    *cap = ncap;
  }
  (*arr)[(*n)++] = x;
  (*arr)[*n] = NULL;
}

static int try_alias(struct parser *P) {
  if (peek(P) != T_WORD || P->lx.quoted) return 0;
  struct alias *a = alias_lookup(P->lx.text);
  if (!a || a->active) return 0;
  consume(P);
  src_push_alias(P->lx.src, a->value, a);
  size_t l = strlen(a->value);
  P->alias_next = l && (a->value[l - 1] == ' ' || a->value[l - 1] == '\t');
  return 1;
}

static struct node *parse_and_or(struct parser *P);
static struct node *parse_cmd(struct parser *P);

/* A list of and-or lists separated by ; & or newlines, ending at a reserved
 * word, ')' , ';;' or end of input. */
static struct node *parse_compound_list(struct parser *P) {
  struct node **items = NULL;
  int n = 0, cap = 0;
  skip_newlines(P);
  if (at_terminator(P)) syntax_error(P);
  for (;;) {
    struct node *item = parse_and_or(P);
    int t = peek(P);
    if (t == T_AMP) {
      struct node *bg = mknode(P, N_BG);
      bg->u.body = item;
      item = bg;
    }
    vpush(P, (void ***)&items, &n, &cap, item);
    if (t != T_AMP && t != T_SEMI && t != T_NL) break;
    consume(P);
    skip_newlines(P);
    if (at_terminator(P)) break;
  }
  if (n == 1) return items[0];
  struct node *list = mknode(P, N_LIST);
  list->u.list.items = items;
  list->u.list.n = n;
  return list;
}

static struct node *parse_pipeline(struct parser *P) {
  int negate = 0;
  while (is_kw(P, "!")) {
    consume(P);
    negate = !negate;
  }
  struct node *first = parse_cmd(P);
  struct node *n = first;
  if (peek(P) == T_PIPE) {
    struct node **items = NULL;
    int cnt = 0, cap = 0;
    vpush(P, (void ***)&items, &cnt, &cap, first);
    while (peek(P) == T_PIPE) {
      consume(P);
      skip_newlines(P);
      vpush(P, (void ***)&items, &cnt, &cap, parse_cmd(P));
    }
    n = mknode(P, N_PIPE);
    n->u.list.items = items;
    n->u.list.n = cnt;
  }
  if (negate) {
    struct node *nn = mknode(P, N_NOT);
    nn->u.body = n;
    n = nn;
  }
  return n;
}

static struct node *parse_and_or(struct parser *P) {
  struct node *left = parse_pipeline(P);
  for (;;) {
    int t = peek(P);
    if (t != T_AND && t != T_OR) return left;
    consume(P);
    skip_newlines(P);
    struct node *n = mknode(P, t == T_AND ? N_AND : N_OR);
    n->u.bin.left = left;
    n->u.bin.right = parse_pipeline(P);
    left = n;
  }
}

static int is_redir_tok(int t) {
  return t == T_IONUM || (t >= T_LESS && t <= T_CLOBBER);
}

static struct redir *parse_redir(struct parser *P) {
  struct redir *r = arena_alloc(P->lx.a, sizeof(*r));
  memset(r, 0, sizeof(*r));
  r->fd = -1;
  if (peek(P) == T_IONUM) {
    r->fd = P->lx.ionum;
    consume(P);
  }
  int t = peek(P);
  consume(P);
  switch (t) {
    case T_LESS:
      r->type = R_IN;
      break;
    case T_GREAT:
      r->type = R_OUT;
      break;
    case T_CLOBBER:
      r->type = R_CLOBBER;
      break;
    case T_DGREAT:
      r->type = R_APPEND;
      break;
    case T_LESSGREAT:
      r->type = R_RDWR;
      break;
    case T_LESSAND:
      r->type = R_DUPIN;
      break;
    case T_GREATAND:
      r->type = R_DUPOUT;
      break;
    case T_DLESS:
    case T_DLESSDASH:
      r->type = R_HEREDOC;
      r->hd_strip = t == T_DLESSDASH;
      break;
    default:
      syntax_error(P);
  }
  if (r->fd < 0)
    r->fd = (r->type == R_IN || r->type == R_RDWR || r->type == R_DUPIN ||
             r->type == R_HEREDOC)
                ? 0
                : 1;
  if (peek(P) != T_WORD) syntax_error(P);
  r->word = P->lx.text;
  consume(P);
  /* register before lexing further, so the body is read at the newline */
  if (r->type == R_HEREDOC) lex_add_heredoc(&P->lx, r);
  return r;
}

static void parse_redirs(struct parser *P, struct node *n) {
  struct redir **tail = &n->redirs;
  while (*tail) tail = &(*tail)->next;
  while (is_redir_tok(peek(P))) {
    *tail = parse_redir(P);
    tail = &(*tail)->next;
  }
}

static struct node *parse_if(struct parser *P) {
  struct node *n = mknode(P, N_IF);
  consume(P); /* if / elif */
  n->u.ifn.cond = parse_compound_list(P);
  expect_kw(P, "then");
  n->u.ifn.then = parse_compound_list(P);
  if (is_kw(P, "elif")) {
    n->u.ifn.els = parse_if(P);
    return n;
  }
  if (is_kw(P, "else")) {
    consume(P);
    n->u.ifn.els = parse_compound_list(P);
  }
  expect_kw(P, "fi");
  return n;
}

static struct node *parse_loop(struct parser *P, int type) {
  struct node *n = mknode(P, type);
  consume(P);
  n->u.loop.cond = parse_compound_list(P);
  expect_kw(P, "do");
  n->u.loop.body = parse_compound_list(P);
  expect_kw(P, "done");
  return n;
}

static struct node *parse_for(struct parser *P) {
  struct node *n = mknode(P, N_FOR);
  consume(P);
  if (peek(P) != T_WORD || !valid_name(P->lx.text)) syntax_error(P);
  n->u.forn.var = P->lx.text;
  consume(P);
  skip_newlines(P);
  if (is_kw(P, "in")) {
    consume(P);
    char **words = NULL;
    int cnt = 0, cap = 0;
    while (peek(P) == T_WORD) {
      vpush(P, (void ***)&words, &cnt, &cap, P->lx.text);
      consume(P);
    }
    if (!words) {
      words = arena_alloc(P->lx.a, sizeof(char *));
      words[0] = NULL;
    }
    n->u.forn.words = words;
    n->u.forn.nwords = cnt;
    if (peek(P) != T_SEMI && peek(P) != T_NL) syntax_error(P);
    consume(P);
  } else if (peek(P) == T_SEMI) {
    consume(P);
  }
  skip_newlines(P);
  expect_kw(P, "do");
  n->u.forn.body = parse_compound_list(P);
  expect_kw(P, "done");
  return n;
}

static struct node *parse_case(struct parser *P) {
  struct node *n = mknode(P, N_CASE);
  consume(P);
  if (peek(P) != T_WORD) syntax_error(P);
  n->u.casen.word = P->lx.text;
  consume(P);
  skip_newlines(P);
  expect_kw(P, "in");
  struct caseitem **tail = &n->u.casen.items;
  for (;;) {
    skip_newlines(P);
    if (is_kw(P, "esac")) {
      consume(P);
      break;
    }
    struct caseitem *ci = arena_alloc(P->lx.a, sizeof(*ci));
    memset(ci, 0, sizeof(*ci));
    int cap = 0;
    if (peek(P) == T_LPAREN) consume(P);
    for (;;) {
      if (peek(P) != T_WORD) syntax_error(P);
      vpush(P, (void ***)&ci->pats, &ci->npats, &cap, P->lx.text);
      consume(P);
      if (peek(P) != T_PIPE) break;
      consume(P);
    }
    expect(P, T_RPAREN);
    skip_newlines(P);
    if (peek(P) != T_DSEMI && !is_kw(P, "esac"))
      ci->body = parse_compound_list(P);
    *tail = ci;
    tail = &ci->next;
    if (peek(P) == T_DSEMI) {
      consume(P);
      continue;
    }
    expect_kw(P, "esac");
    break;
  }
  return n;
}

static int is_compound_start(struct parser *P) {
  static const char *const kws[] = {"{",     "if",   "while", "until",
                                    "for",   "case", NULL};
  if (peek(P) == T_LPAREN) return 1;
  if (peek(P) != T_WORD || P->lx.quoted) return 0;
  for (int i = 0; kws[i]; i++)
    if (strcmp(P->lx.text, kws[i]) == 0) return 1;
  return 0;
}

static struct node *parse_compound(struct parser *P) {
  struct node *n;
  if (peek(P) == T_LPAREN) {
    consume(P);
    n = mknode(P, N_SUBSHELL);
    n->u.body = parse_compound_list(P);
    expect(P, T_RPAREN);
  } else if (is_kw(P, "{")) {
    consume(P);
    n = mknode(P, N_GROUP);
    n->u.body = parse_compound_list(P);
    expect_kw(P, "}");
  } else if (is_kw(P, "if")) {
    n = parse_if(P);
  } else if (is_kw(P, "while")) {
    n = parse_loop(P, N_WHILE);
  } else if (is_kw(P, "until")) {
    n = parse_loop(P, N_UNTIL);
  } else if (is_kw(P, "for")) {
    n = parse_for(P);
  } else {
    n = parse_case(P);
  }
  parse_redirs(P, n);
  return n;
}

static int is_assignment(const char *w) {
  size_t n = name_len(w);
  return n > 0 && w[n] == '=';
}

static struct node *parse_simple(struct parser *P) {
  struct node *n = mknode(P, N_SIMPLE);
  struct redir **rtail = &n->redirs;
  int argcap = 0, ascap = 0;
  for (;;) {
    int t = peek(P);
    if (is_redir_tok(t)) {
      *rtail = parse_redir(P);
      rtail = &(*rtail)->next;
      continue;
    }
    if (t != T_WORD) break;
    if (n->u.simple.argc == 0 && is_assignment(P->lx.text)) {
      vpush(P, (void ***)&n->u.simple.assigns, &n->u.simple.nassigns, &ascap,
            P->lx.text);
      consume(P);
      continue;
    }
    if (n->u.simple.argc == 0 && n->u.simple.nassigns > 0 && try_alias(P))
      continue;
    if (n->u.simple.argc > 0 && P->alias_next) {
      P->alias_next = 0;
      if (try_alias(P)) continue;
    }
    vpush(P, (void ***)&n->u.simple.argv, &n->u.simple.argc, &argcap,
          P->lx.text);
    consume(P);
    if (n->u.simple.argc == 1 && n->u.simple.nassigns == 0 && !n->redirs &&
        peek(P) == T_LPAREN) {
      /* function definition: name ( ) compound-command */
      char *name = n->u.simple.argv[0];
      if (!valid_name(name)) syntax_error(P);
      consume(P);
      expect(P, T_RPAREN);
      skip_newlines(P);
      while (try_alias(P)) {
      }
      if (!is_compound_start(P)) syntax_error(P);
      struct node *f = mknode(P, N_FUNCDEF);
      f->u.func.name = name;
      f->u.func.body = parse_compound(P);
      f->u.func.a = P->lx.a;
      return f;
    }
  }
  if (n->u.simple.argc == 0 && n->u.simple.nassigns == 0 && !n->redirs)
    syntax_error(P);
  P->alias_next = 0;
  return n;
}

static struct node *parse_cmd(struct parser *P) {
  while (try_alias(P)) {
  }
  if (is_compound_start(P)) return parse_compound(P);
  if (peek(P) == T_WORD && !P->lx.quoted && at_terminator(P)) syntax_error(P);
  return parse_simple(P);
}

struct node *parse_command(struct source *src, arena **ap, int *eof) {
  struct parser P;
  arena *a = arena_new();
  parse_arena = a;
  *ap = a;
  *eof = 0;
  P.peeked = 0;
  P.alias_next = 0;
  lex_init(&P.lx, src, a);

  struct node *result = NULL;
  int t = peek(&P);
  if (t == T_EOF) {
    *eof = 1;
  } else if (t == T_NL) {
    consume(&P);
  } else {
    struct node **items = NULL;
    int n = 0, cap = 0;
    for (;;) {
      struct node *item = parse_and_or(&P);
      t = peek(&P);
      if (t == T_AMP) {
        struct node *bg = mknode(&P, N_BG);
        bg->u.body = item;
        item = bg;
      }
      vpush(&P, (void ***)&items, &n, &cap, item);
      if (t == T_AMP || t == T_SEMI) {
        consume(&P);
        t = peek(&P);
        if (t == T_NL || t == T_EOF) break;
        continue;
      }
      if (t == T_NL || t == T_EOF) break;
      syntax_error(&P);
    }
    if (peek(&P) == T_NL) consume(&P);
    if (n == 1) {
      result = items[0];
    } else {
      result = mknode(&P, N_LIST);
      result->u.list.items = items;
      result->u.list.n = n;
    }
  }
  lex_done(&P.lx);
  src->nextprompt = 1;
  return result;
}
