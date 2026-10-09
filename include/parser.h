#ifndef PARSER_H
#define PARSER_H

#include "common.h"
#include "input.h"

enum node_type {
  N_SIMPLE,   /* simple command */
  N_PIPE,     /* a | b | c */
  N_AND,      /* a && b */
  N_OR,       /* a || b */
  N_LIST,     /* a; b; c */
  N_BG,       /* a & */
  N_NOT,      /* ! a */
  N_SUBSHELL, /* ( list ) */
  N_GROUP,    /* { list; } */
  N_IF,
  N_WHILE,
  N_UNTIL,
  N_FOR,
  N_CASE,
  N_FUNCDEF
};

enum redir_type {
  R_IN,      /* <  */
  R_OUT,     /* >  */
  R_CLOBBER, /* >| */
  R_APPEND,  /* >> */
  R_RDWR,    /* <> */
  R_DUPIN,   /* <& */
  R_DUPOUT,  /* >& */
  R_HEREDOC  /* << and <<- */
};

struct redir {
  struct redir *next;
  int type;
  int fd;
  char *word;    /* target, or here-document delimiter */
  char *heredoc; /* here-document body */
  int hd_quoted; /* delimiter was quoted: body is not expanded */
  int hd_strip;  /* <<- */
  struct redir *hdnext; /* parser: here-documents awaiting a body */
};

struct node;

struct caseitem {
  struct caseitem *next;
  char **pats;
  int npats;
  struct node *body; /* may be NULL */
};

struct node {
  int type;
  int lineno;
  struct redir *redirs;
  union {
    struct {
      char **argv;
      int argc;
      char **assigns;
      int nassigns;
    } simple;
    struct {
      struct node **items;
      int n;
    } list; /* N_PIPE, N_LIST */
    struct {
      struct node *left, *right;
    } bin; /* N_AND, N_OR */
    struct node *body; /* N_BG, N_NOT, N_SUBSHELL, N_GROUP */
    struct {
      struct node *cond, *then, *els;
    } ifn;
    struct {
      struct node *cond, *body;
    } loop;
    struct {
      char *var;
      char **words; /* NULL when there is no `in`: iterate "$@" */
      int nwords;
      struct node *body;
    } forn;
    struct {
      char *word;
      struct caseitem *items;
    } casen;
    struct {
      char *name;
      struct node *body;
      arena *a; /* arena holding the body; functions keep a reference */
    } func;
  } u;
};

/* Parse one complete command (through the end of its line) from src.
 * The tree is allocated in a fresh arena returned via *ap; the caller owns
 * that reference. Returns NULL for blank lines; sets *eof at end of input.
 * Syntax errors call sh_error(). */
struct node *parse_command(struct source *src, arena **ap, int *eof);

/* Parse the body of a command substitution, just after "$(", through its
 * closing ')'. Only the extent matters: the text is parsed again when the
 * substitution runs. Nodes go in arena a. Syntax errors call sh_error(). */
void parse_cmdsub(struct source *src, arena *a);

/* The arena of the command currently being parsed, so an error handler can
 * release it after longjmp. */
extern arena *parse_arena;

/* Scanning helpers shared with the expander. Each takes a pointer just past
 * the opening delimiter and returns a pointer just past the closing one. */
const char *scan_squote(const char *p);
const char *scan_dquote(const char *p);
const char *scan_backquote(const char *p);
const char *scan_dollar(const char *p); /* p points at the '$' */

#endif
