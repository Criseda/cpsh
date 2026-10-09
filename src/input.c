#include "input.h"

#include "alias.h"
#include "history.h"
#include "lineedit.h"
#include "prompt.h"
#include "shell.h"
#include "trap.h"

#define BUFSZ 8192

struct source *src_string(const char *s) {
  struct source *src = xcalloc(1, sizeof(*src));
  src->fd = -1;
  src->str = s;
  src->lineno = 1;
  src->nextprompt = 1;
  return src;
}

struct source *src_fd(int fd, int interactive) {
  struct source *src = xcalloc(1, sizeof(*src));
  src->fd = fd;
  src->interactive = interactive;
  src->lineno = 1;
  src->nextprompt = 1;
  if (!interactive) {
    /* stdin is shared with the commands we run, so we must not read past
     * the end of the current command: seek back if we can, otherwise read
     * one byte at a time. Other descriptors are private to the shell. */
    if (fd == 0) {
      if (lseek(fd, 0, SEEK_CUR) >= 0)
        src->seekable = 1;
      else
        src->unbuffered = 1;
    }
    src->buf = xmalloc(BUFSZ);
  }
  return src;
}

void src_free(struct source *s) {
  while (s->push) {
    struct strpush *sp = s->push;
    s->push = sp->prev;
    if (sp->alias) sp->alias->active--;
    free(sp->s);
    free(sp);
  }
  src_rec_end(s);
  free(s->buf);
  free(s->line);
  sb_free(&s->hist);
  free(s);
}

static int fetch_line(struct source *s) {
  free(s->line);
  s->line = NULL;
  s->linepos = s->linelen = 0;
  if (s->nextprompt == 1) s->hist.len = 0;
  char *prompt = prompt_string(s->nextprompt);
  char *line = lineedit_read(prompt);
  free(prompt);
  if (!line) {
    if (errno == EINTR) raise_exception(EX_INT);
    return 0;
  }
  if (strchr(line, '!')) {
    int changed;
    char *exp = history_expand(line, &changed);
    free(line);
    if (!exp) {
      /* event not found: drop the whole command */
      s->hist.len = 0;
      raise_exception(EX_INT);
    }
    if (changed) fputs(exp, stderr);
    line = exp;
  }
  s->line = line;
  s->linelen = strlen(line);
  sb_puts(&s->hist, line);
  s->nextprompt = 2;
  return 1;
}

static int fill(struct source *s) {
  for (;;) {
    ssize_t n = read(s->fd, s->buf, s->unbuffered ? 1 : BUFSZ);
    if (n < 0 && errno == EINTR) {
      dotrap();
      continue;
    }
    if (n <= 0) return 0;
    s->bufpos = 0;
    s->buflen = (size_t)n;
    return 1;
  }
}

static int getc_raw(struct source *s) {
  int c;
  if (s->nunget) return s->unget[--s->nunget];
  while (s->push) {
    struct strpush *sp = s->push;
    if (sp->s[sp->pos]) return (unsigned char)sp->s[sp->pos++];
    s->push = sp->prev;
    if (sp->alias) sp->alias->active--;
    free(sp->s);
    free(sp);
  }
  if (s->eof) return PEOF;
  if (s->fd < 0) {
    c = (unsigned char)s->str[s->strpos];
    if (!c) {
      s->eof = 1;
      return PEOF;
    }
    s->strpos++;
  } else if (s->interactive) {
    if (s->linepos >= s->linelen && !fetch_line(s)) {
      s->eof = 1;
      return PEOF;
    }
    c = (unsigned char)s->line[s->linepos++];
  } else {
    if (s->bufpos >= s->buflen && !fill(s)) {
      s->eof = 1;
      return PEOF;
    }
    c = (unsigned char)s->buf[s->bufpos++];
  }
  if (c == '\n') s->lineno++;
  if (vflag && s->fd >= 0) { /* set -v: echo input as it is read */
    char ch = (char)c;
    xwrite(2, &ch, 1);
  }
  return c;
}

int src_getc(struct source *s) {
  int c = getc_raw(s);
  if (s->rec && c != PEOF) sb_putc(s->rec, (char)c);
  return c;
}

void src_ungetc(struct source *s, int c) {
  if (s->nunget < 4) s->unget[s->nunget++] = c;
  if (s->rec && s->rec->len) s->rec->s[--s->rec->len] = '\0';
}

void src_rec_end(struct source *s) {
  if (!s->rec) return;
  sb_free(s->rec);
  free(s->rec);
  s->rec = NULL;
}

void src_push_alias(struct source *s, const char *text, struct alias *a) {
  struct strpush *sp = xmalloc(sizeof(*sp));
  /* Pending lookahead characters belong after the alias text. */
  size_t tl = strlen(text);
  sp->s = xmalloc(tl + s->nunget + 1);
  memcpy(sp->s, text, tl);
  size_t k = tl;
  while (s->nunget) sp->s[k++] = (char)s->unget[--s->nunget];
  sp->s[k] = '\0';
  sp->pos = 0;
  sp->alias = a;
  if (a) a->active++;
  sp->prev = s->push;
  s->push = sp;
}

void src_sync(struct source *s) {
  if (!s->seekable || s->bufpos >= s->buflen) return;
  off_t back = (off_t)(s->buflen - s->bufpos);
  if (lseek(s->fd, -back, SEEK_CUR) >= 0) s->bufpos = s->buflen = 0;
}

void src_reset(struct source *s) {
  src_rec_end(s);
  s->nunget = 0;
  while (s->push) {
    struct strpush *sp = s->push;
    s->push = sp->prev;
    if (sp->alias) sp->alias->active--;
    free(sp->s);
    free(sp);
  }
  if (s->interactive) {
    s->linepos = s->linelen;
    s->eof = 0;
  }
  s->nextprompt = 1;
}
