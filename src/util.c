#include "common.h"

#include <fnmatch.h>
#include <locale.h>

const char *progname = CPSH_NAME;

/* ---- checked allocation ---- */

static NORETURN void nomem(void) {
  static const char msg[] = CPSH_NAME ": out of memory\n";
  xwrite(2, msg, sizeof(msg) - 1);
  _exit(2);
}

void *xmalloc(size_t n) {
  void *p = malloc(n ? n : 1);
  if (!p) nomem();
  return p;
}

void *xcalloc(size_t n, size_t size) {
  void *p = calloc(n ? n : 1, size ? size : 1);
  if (!p) nomem();
  return p;
}

void *xrealloc(void *p, size_t n) {
  p = realloc(p, n ? n : 1);
  if (!p) nomem();
  return p;
}

char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

char *xstrndup(const char *s, size_t n) {
  char *p = xmalloc(n + 1);
  memcpy(p, s, n);
  p[n] = '\0';
  return p;
}

/* ---- growable string ---- */

void sb_init(strbuf *sb) {
  sb->s = NULL;
  sb->len = sb->cap = 0;
}

static void sb_reserve(strbuf *sb, size_t extra) {
  if (sb->len + extra + 1 <= sb->cap) return;
  size_t cap = sb->cap ? sb->cap : 64;
  while (cap < sb->len + extra + 1) cap *= 2;
  sb->s = xrealloc(sb->s, cap);
  sb->cap = cap;
}

void sb_putc(strbuf *sb, char c) {
  sb_reserve(sb, 1);
  sb->s[sb->len++] = c;
  sb->s[sb->len] = '\0';
}

void sb_putn(strbuf *sb, const char *s, size_t n) {
  sb_reserve(sb, n);
  memcpy(sb->s + sb->len, s, n);
  sb->len += n;
  sb->s[sb->len] = '\0';
}

void sb_puts(strbuf *sb, const char *s) { sb_putn(sb, s, strlen(s)); }

char *sb_detach(strbuf *sb) {
  char *s = sb->s ? sb->s : xstrdup("");
  sb_init(sb);
  return s;
}

void sb_free(strbuf *sb) {
  free(sb->s);
  sb_init(sb);
}

/* ---- arenas ---- */

#define ALIGN(n) (((n) + 15) & ~(size_t)15)
#define CHUNK_SIZE 8192

struct chunk {
  struct chunk *prev;
  size_t size;
  /* payload follows, 16-byte aligned */
};

#define CHUNK_DATA(c) ((char *)(c) + ALIGN(sizeof(struct chunk)))

struct arena {
  int refcnt;
  struct chunk *cur;
  size_t off;
  struct chunk *spare; /* one cached chunk to avoid malloc churn */
};

arena *arena_new(void) {
  arena *a = xcalloc(1, sizeof(*a));
  a->refcnt = 1;
  return a;
}

void arena_ref(arena *a) { a->refcnt++; }

void arena_unref(arena *a) {
  if (!a || --a->refcnt > 0) return;
  struct chunk *c = a->cur;
  while (c) {
    struct chunk *prev = c->prev;
    free(c);
    c = prev;
  }
  free(a->spare);
  free(a);
}

static void arena_newchunk(arena *a, size_t need) {
  struct chunk *c;
  size_t size = need > CHUNK_SIZE ? need : CHUNK_SIZE;
  if (a->spare && a->spare->size >= size) {
    c = a->spare;
    a->spare = NULL;
  } else {
    c = xmalloc(ALIGN(sizeof(struct chunk)) + size);
    c->size = size;
  }
  c->prev = a->cur;
  a->cur = c;
  a->off = 0;
}

void *arena_alloc(arena *a, size_t n) {
  n = ALIGN(n ? n : 1);
  if (!a->cur || a->cur->size - a->off < n) arena_newchunk(a, n);
  void *p = CHUNK_DATA(a->cur) + a->off;
  a->off += n;
  return p;
}

char *arena_strndup(arena *a, const char *s, size_t n) {
  char *p = arena_alloc(a, n + 1);
  memcpy(p, s, n);
  p[n] = '\0';
  return p;
}

char *arena_strdup(arena *a, const char *s) {
  return arena_strndup(a, s, strlen(s));
}

/* ---- scratch stack ---- */

arena *scratch;

void *stalloc(size_t n) { return arena_alloc(scratch, n); }
char *ststrdup(const char *s) { return arena_strdup(scratch, s); }
char *ststrndup(const char *s, size_t n) {
  return arena_strndup(scratch, s, n);
}

/* Grow the block p (oldn bytes) to newn bytes; extends in place when p is the
 * most recent allocation, otherwise copies. */
void *stgrow(void *p, size_t oldn, size_t newn) {
  arena *a = scratch;
  if (p && a->cur) {
    char *base = CHUNK_DATA(a->cur);
    size_t start = (size_t)((char *)p - base);
    if ((char *)p >= base && start + ALIGN(oldn ? oldn : 1) == a->off &&
        a->cur->size - start >= newn) {
      a->off = start + ALIGN(newn ? newn : 1);
      return p;
    }
  }
  void *q = arena_alloc(a, newn);
  if (p) memcpy(q, p, oldn < newn ? oldn : newn);
  return q;
}

stackmark stmark(void) {
  stackmark m;
  m.chunk = scratch->cur;
  m.off = scratch->off;
  return m;
}

void strelease(stackmark m) {
  arena *a = scratch;
  while (a->cur && a->cur != m.chunk) {
    struct chunk *c = a->cur;
    a->cur = c->prev;
    if (!a->spare || a->spare->size < c->size) {
      free(a->spare);
      a->spare = c;
    } else {
      free(c);
    }
  }
  a->off = m.chunk ? m.off : 0;
}

/* ---- errors ---- */

struct jmploc *handler;
int exception;

void raise_exception(int e) {
  exception = e;
  if (handler) longjmp(handler->buf, 1);
  fflush(stdout);
  _exit(2);
}

static void vwarn(const char *fmt, va_list ap) {
  fflush(stdout);
  fprintf(stderr, "%s: ", progname);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  fflush(stderr);
}

void sh_warn(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vwarn(fmt, ap);
  va_end(ap);
}

void sh_error(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vwarn(fmt, ap);
  va_end(ap);
  raise_exception(EX_ERROR);
}

/* ---- misc ---- */

int is_number(const char *s) {
  if (!*s) return 0;
  for (; *s; s++)
    if (*s < '0' || *s > '9') return 0;
  return 1;
}

int xwrite(int fd, const void *buf, size_t n) {
  const char *p = buf;
  while (n > 0) {
    ssize_t w = write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    p += w;
    n -= (size_t)w;
  }
  return 0;
}

int move_fd_high(int fd) {
  if (fd >= 10) {
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
  }
  int nfd = fcntl(fd, F_DUPFD_CLOEXEC, 10);
  if (nfd < 0) return fd;
  close(fd);
  return nfd;
}

void sh_quote(strbuf *sb, const char *s) {
  const char *p;
  int safe = *s != '\0';
  for (p = s; *p && safe; p++)
    if (!(isalnum((unsigned char)*p) || strchr("@%+=:,./-_", *p))) safe = 0;
  if (safe) {
    sb_puts(sb, s);
    return;
  }
  sb_putc(sb, '\'');
  for (p = s; *p; p++) {
    if (*p == '\'')
      sb_puts(sb, "'\\''");
    else
      sb_putc(sb, *p);
  }
  sb_putc(sb, '\'');
}

/* ---- pattern matching ---- */

/* The user's locale with C collation, or 0 if it cannot be made. The
 * shell sets its locale once at startup, so it is made once. */
static locale_t match_locale(void) {
  static int made;
  static locale_t loc;
  if (!made) {
    made = 1;
    locale_t base = duplocale(LC_GLOBAL_LOCALE);
    if (base) {
      loc = newlocale(LC_COLLATE_MASK, "C", base);
      if (!loc) freelocale(base);
    }
  }
  return loc;
}

int pmatch(const char *pat, const char *s) {
  locale_t loc = match_locale();
  locale_t old = loc ? uselocale(loc) : 0;
  int r = fnmatch(pat, s, 0);
  if (loc) uselocale(old);
  return r == 0;
}

static int collate_cmp(const void *a, const void *b) {
  return strcoll(*(char *const *)a, *(char *const *)b);
}

int pglob(const char *pat, glob_t *g) {
  locale_t loc = match_locale();
  locale_t old = loc ? uselocale(loc) : 0;
  int r = glob(pat, loc ? GLOB_NOSORT : 0, NULL, g);
  if (loc) uselocale(old);
  if (r == 0 && loc)
    qsort(g->gl_pathv, g->gl_pathc, sizeof(char *), collate_cmp);
  return r;
}
