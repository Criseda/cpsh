#ifndef COMMON_H
#define COMMON_H

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CPSH_NAME "cpsh"
#define CPSH_VERSION "1.0.0"

#if defined(__GNUC__)
#define NORETURN __attribute__((noreturn))
#define PRINTFLIKE(a, b) __attribute__((format(printf, a, b)))
#else
#define NORETURN
#define PRINTFLIKE(a, b)
#endif

/* ---- checked allocation ---- */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* ---- growable string ---- */
typedef struct {
  char *s;
  size_t len, cap;
} strbuf;

void sb_init(strbuf *sb);
void sb_putc(strbuf *sb, char c);
void sb_putn(strbuf *sb, const char *s, size_t n);
void sb_puts(strbuf *sb, const char *s);
char *sb_detach(strbuf *sb); /* returns malloc'd string, resets sb */
void sb_free(strbuf *sb);

/* ---- arenas ----
 * Arenas are bump allocators. The parser allocates each command's syntax tree
 * in a reference-counted arena (functions keep a reference to theirs). The
 * global `scratch` arena holds short-lived data (expanded words, argv arrays)
 * and is released in LIFO order with stmark()/strelease(), so nothing leaks
 * even when an error unwinds the stack with longjmp. */
typedef struct arena arena;
typedef struct {
  void *chunk;
  size_t off;
} stackmark;

arena *arena_new(void);
void arena_ref(arena *a);
void arena_unref(arena *a);
void *arena_alloc(arena *a, size_t n);
char *arena_strdup(arena *a, const char *s);
char *arena_strndup(arena *a, const char *s, size_t n);

extern arena *scratch;
void *stalloc(size_t n);
char *ststrdup(const char *s);
char *ststrndup(const char *s, size_t n);
void *stgrow(void *p, size_t oldn, size_t newn);
stackmark stmark(void);
void strelease(stackmark m);

/* ---- errors ----
 * sh_error() prints a diagnostic and unwinds to the innermost handler with
 * longjmp. In a forked child (handler == NULL) it exits instead. */
struct jmploc {
  jmp_buf buf;
};
extern struct jmploc *handler;

#define EX_ERROR 1 /* shell error: non-interactive shells exit */
#define EX_INT 2   /* interrupted */

extern int exception;
extern const char *progname; /* prefix for diagnostics */
NORETURN void raise_exception(int e);
NORETURN void sh_error(const char *fmt, ...) PRINTFLIKE(1, 2);
void sh_warn(const char *fmt, ...) PRINTFLIKE(1, 2);

/* ---- misc helpers ---- */
int is_number(const char *s);
int xwrite(int fd, const void *buf, size_t n);
int move_fd_high(int fd); /* dup to >= 10 with FD_CLOEXEC, closes old */
void sh_quote(strbuf *sb, const char *s); /* single-quote for re-input */

#endif
