#ifndef INPUT_H
#define INPUT_H

#include "common.h"

#define PEOF (-1)

struct alias;

/* Text pushed in front of the input, used for alias substitution. */
struct strpush {
  struct strpush *prev;
  char *s;
  size_t pos;
  struct alias *alias;
};

struct source {
  struct source *prev;
  int fd;          /* -1 for string sources */
  const char *str; /* string source */
  size_t strpos;
  char *buf; /* fd buffer */
  size_t bufpos, buflen;
  int unbuffered;  /* read byte-at-a-time (shared, unseekable stdin) */
  int seekable;    /* stdin we can lseek back to share with children */
  int interactive; /* read lines through the line editor */
  char *line;      /* current interactive line */
  size_t linepos, linelen;
  strbuf hist; /* interactive: text of the command being read */
  struct strpush *push;
  int unget[4];
  int nunget;
  int lineno;
  int nextprompt; /* 1 = PS1, 2 = PS2 */
  int eof;
};

struct source *src_string(const char *s);
struct source *src_fd(int fd, int interactive);
void src_free(struct source *s);
int src_getc(struct source *s);
void src_ungetc(struct source *s, int c);
void src_push_alias(struct source *s, const char *text, struct alias *a);
int src_alias_active(struct source *s, struct alias *a);
/* Return unread buffered input to the file so children see it. */
void src_sync(struct source *s);
/* Drop the remainder of the current line (after an error/interrupt). */
void src_reset(struct source *s);

#endif
