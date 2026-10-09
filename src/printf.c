/* printf built-in (POSIX). */
#include "builtins.h"
#include "common.h"

static char **pargs;
static int pnargs, pidx, pstatus;

static const char *next_arg(void) {
  if (pidx < pnargs) return pargs[pidx++];
  return NULL;
}

static void conv_error(const char *arg) {
  sh_warn("printf: %s: invalid number", arg);
  pstatus = 1;
}

static long arg_long(void) {
  const char *a = next_arg();
  if (!a) return 0;
  if (*a == '\'' || *a == '"') return (unsigned char)a[1];
  char *end;
  errno = 0;
  long v = strtol(a, &end, 0);
  if (end == a || *end || errno) conv_error(a);
  return v;
}

static unsigned long arg_ulong(void) {
  const char *a = next_arg();
  if (!a) return 0;
  if (*a == '\'' || *a == '"') return (unsigned char)a[1];
  char *end;
  errno = 0;
  unsigned long v = *a == '-' ? (unsigned long)strtol(a, &end, 0)
                              : strtoul(a, &end, 0);
  if (end == a || *end || errno) conv_error(a);
  return v;
}

static double arg_double(void) {
  const char *a = next_arg();
  if (!a) return 0;
  if (*a == '\'' || *a == '"') return (unsigned char)a[1];
  char *end;
  errno = 0;
  double v = strtod(a, &end);
  if (end == a || *end) conv_error(a);
  return v;
}

/* Process one backslash escape at *pp (pointing after the backslash).
 * Returns 0 if output must stop (\c in %b). */
static int escape(const char **pp, strbuf *out, int in_b) {
  const char *p = *pp;
  int c = *p;
  switch (c) {
    case '\\': sb_putc(out, '\\'); break;
    case 'a': sb_putc(out, '\a'); break;
    case 'b': sb_putc(out, '\b'); break;
    case 'f': sb_putc(out, '\f'); break;
    case 'n': sb_putc(out, '\n'); break;
    case 'r': sb_putc(out, '\r'); break;
    case 't': sb_putc(out, '\t'); break;
    case 'v': sb_putc(out, '\v'); break;
    case 'c':
      if (in_b) {
        *pp = p + 1;
        return 0;
      }
      sb_putc(out, '\\');
      sb_putc(out, 'c');
      break;
    case '0': case '1': case '2': case '3':
    case '4': case '5': case '6': case '7': {
      /* \0ddd in %b, \ddd in the format */
      int max = in_b && c == '0' ? 4 : 3, v = 0, k = 0;
      while (k < max && *p >= '0' && *p <= '7') {
        v = v * 8 + (*p - '0');
        p++;
        k++;
      }
      sb_putc(out, (char)v);
      *pp = p;
      return 1;
    }
    case '\0':
      sb_putc(out, '\\');
      *pp = p;
      return 1;
    default:
      sb_putc(out, '\\');
      sb_putc(out, (char)c);
  }
  *pp = p + 1;
  return 1;
}

static void put_formatted(strbuf *out, const char *spec, char conv) {
  char buf[512];
  char *dyn = NULL;
  int n;
  switch (conv) {
    case 'd':
    case 'i': {
      char f[64];
      snprintf(f, sizeof(f), "%sl%c", spec, conv);
      n = snprintf(buf, sizeof(buf), f, arg_long());
      if (n >= (int)sizeof(buf)) {
        dyn = xmalloc((size_t)n + 1);
        pidx--;
        snprintf(dyn, (size_t)n + 1, f, arg_long());
      }
      break;
    }
    case 'o':
    case 'u':
    case 'x':
    case 'X': {
      char f[64];
      snprintf(f, sizeof(f), "%sl%c", spec, conv);
      n = snprintf(buf, sizeof(buf), f, arg_ulong());
      if (n >= (int)sizeof(buf)) {
        dyn = xmalloc((size_t)n + 1);
        pidx--;
        snprintf(dyn, (size_t)n + 1, f, arg_ulong());
      }
      break;
    }
    case 'c': {
      /* as %s of the first character, so an empty argument prints only
       * the padding */
      const char *a = next_arg();
      char ch[2] = {a ? *a : '\0', '\0'};
      char f[64];
      snprintf(f, sizeof(f), "%ss", spec);
      n = snprintf(buf, sizeof(buf), f, ch);
      if (n >= (int)sizeof(buf)) {
        dyn = xmalloc((size_t)n + 1);
        snprintf(dyn, (size_t)n + 1, f, ch);
      }
      break;
    }
    case 's': {
      const char *a = next_arg();
      char f[64];
      snprintf(f, sizeof(f), "%ss", spec);
      n = snprintf(NULL, 0, f, a ? a : "");
      dyn = xmalloc((size_t)n + 1);
      snprintf(dyn, (size_t)n + 1, f, a ? a : "");
      break;
    }
    default: { /* floating point */
      char f[64];
      snprintf(f, sizeof(f), "%s%c", spec, conv);
      double d = arg_double();
      n = snprintf(NULL, 0, f, d);
      dyn = xmalloc((size_t)n + 1);
      snprintf(dyn, (size_t)n + 1, f, d);
      break;
    }
  }
  if (dyn) {
    sb_putn(out, dyn, strlen(dyn));
    free(dyn);
  } else if (n > 0) {
    sb_putn(out, buf, (size_t)n);
  }
}

/* One pass over the format. Returns 0 if \c stopped output. */
static int do_format(const char *fmt, strbuf *out) {
  for (const char *p = fmt; *p;) {
    if (*p == '\\') {
      p++;
      if (!escape(&p, out, 0)) return 0;
      continue;
    }
    if (*p != '%') {
      sb_putc(out, *p++);
      continue;
    }
    if (p[1] == '%') {
      sb_putc(out, '%');
      p += 2;
      continue;
    }
    /* build a C conversion spec: %[flags][width][.precision] */
    char spec[48];
    size_t k = 0;
    spec[k++] = *p++;
    while (*p && strchr("-+ #0", *p) && k < 10) spec[k++] = *p++;
    if (*p == '*') {
      k += (size_t)snprintf(spec + k, sizeof(spec) - k, "%ld", arg_long());
      p++;
    } else {
      while (isdigit((unsigned char)*p) && k < 20) spec[k++] = *p++;
    }
    if (*p == '.') {
      spec[k++] = *p++;
      if (*p == '*') {
        k += (size_t)snprintf(spec + k, sizeof(spec) - k, "%ld", arg_long());
        p++;
      } else {
        while (isdigit((unsigned char)*p) && k < 40) spec[k++] = *p++;
      }
    }
    spec[k] = '\0';
    char conv = *p;
    if (!conv) {
      sh_warn("printf: missing format character");
      pstatus = 1;
      return 0;
    }
    p++;
    if (conv == 'b') {
      const char *a = next_arg();
      strbuf tmp;
      sb_init(&tmp);
      int go = 1;
      for (const char *q = a ? a : ""; *q && go;) {
        if (*q == '\\') {
          q++;
          go = escape(&q, &tmp, 1);
        } else {
          sb_putc(&tmp, *q++);
        }
      }
      /* apply width/precision as for %s */
      char f[64];
      snprintf(f, sizeof(f), "%ss", spec);
      int n = snprintf(NULL, 0, f, tmp.s ? tmp.s : "");
      char *s = xmalloc((size_t)n + 1);
      snprintf(s, (size_t)n + 1, f, tmp.s ? tmp.s : "");
      sb_puts(out, s);
      free(s);
      sb_free(&tmp);
      if (!go) return 0;
      continue;
    }
    if (!strchr("diouxXcseEfFgGaA", conv)) {
      sh_warn("printf: %%%c: invalid conversion", conv);
      pstatus = 1;
      return 0;
    }
    put_formatted(out, spec, conv);
  }
  return 1;
}

int printf_builtin(int argc, char **argv) {
  int i = 1;
  if (i < argc && strcmp(argv[i], "--") == 0) i++;
  if (i >= argc) {
    sh_warn("printf: usage: printf format [arguments]");
    return 2;
  }
  const char *fmt = argv[i];
  pargs = argv + i + 1;
  pnargs = argc - i - 1;
  pidx = 0;
  pstatus = 0;
  strbuf out;
  sb_init(&out);
  for (;;) {
    int before = pidx;
    if (!do_format(fmt, &out)) break;
    /* reuse the format while arguments remain (and it consumes some) */
    if (pidx >= pnargs || pidx == before) break;
  }
  if (out.len) fwrite(out.s, 1, out.len, stdout);
  sb_free(&out);
  return pstatus;
}
