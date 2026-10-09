#ifndef LEXER_H
#define LEXER_H

#include "parser.h"

enum token {
  T_EOF,
  T_NL,
  T_WORD,
  T_IONUM,
  T_SEMI,      /* ; */
  T_AMP,       /* & */
  T_AND,       /* && */
  T_OR,        /* || */
  T_PIPE,      /* | */
  T_LPAREN,    /* ( */
  T_RPAREN,    /* ) */
  T_DSEMI,     /* ;; */
  T_LESS,      /* < */
  T_GREAT,     /* > */
  T_DLESS,     /* << */
  T_DLESSDASH, /* <<- */
  T_DGREAT,    /* >> */
  T_LESSAND,   /* <& */
  T_GREATAND,  /* >& */
  T_LESSGREAT, /* <> */
  T_CLOBBER,   /* >| */
  T_SEMIAND    /* ;& */
};

struct lexer {
  struct source *src;
  arena *a;
  int tok;
  char *text;  /* T_WORD: raw word text (quotes kept) */
  int quoted;  /* T_WORD contained quoting characters */
  int ionum;   /* T_IONUM value */
  int lineno;  /* line the token started on */
  struct redir *hd_head; /* here-documents awaiting their bodies */
  struct redir **hd_tail;
  char *wbuf; /* word being read, on the scratch stack */
  size_t wlen, wcap;
};

void lex_init(struct lexer *lx, struct source *src, arena *a);
void lex_next(struct lexer *lx);
void lex_add_heredoc(struct lexer *lx, struct redir *r);
const char *tok_str(int tok);

#endif
