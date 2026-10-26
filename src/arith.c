/* Arithmetic expansion: $(( expression )) with C integer semantics. */
#include "expand.h"
#include "vars.h"

struct ap {
  const char *p;
  int noeval; /* >0 inside an unevaluated branch of && || ?: */
};

/* Two's-complement wrap-around, as other shells do, without the undefined
 * behaviour of signed overflow in C. */
static long wrap(unsigned long v) { return (long)v; }
static long wadd(long a, long b) { return wrap((unsigned long)a + (unsigned long)b); }
static long wsub(long a, long b) { return wrap((unsigned long)a - (unsigned long)b); }
static long wmul(long a, long b) { return wrap((unsigned long)a * (unsigned long)b); }
static long wneg(long a) { return wrap(0UL - (unsigned long)a); }

static NORETURN void arith_error(const char *msg) {
  sh_error("arithmetic expression: %s", msg);
}

static void skipws(struct ap *a) {
  while (*a->p == ' ' || *a->p == '\t' || *a->p == '\n') a->p++;
}

static long parse_constant(const char *s, int *ok) {
  char *end;
  while (*s == ' ' || *s == '\t' || *s == '\n') s++;
  if (!*s) {
    *ok = 1;
    return 0;
  }
  errno = 0;
  long v = strtol(s, &end, 0);
  while (*end == ' ' || *end == '\t' || *end == '\n') end++;
  *ok = *end == '\0' && errno == 0;
  return v;
}

static long var_value(const char *name) {
  const char *v = var_get(name);
  if (!v) return 0;
  int ok;
  long n = parse_constant(v, &ok);
  if (!ok) sh_error("%s: bad number '%s'", name, v);
  return n;
}

static long expr_assign(struct ap *a);

static long primary(struct ap *a) {
  skipws(a);
  const char *p = a->p;
  if (*p == '(') {
    a->p++;
    long v = expr_assign(a);
    skipws(a);
    if (*a->p != ')') arith_error("missing ')'");
    a->p++;
    return v;
  }
  if (isdigit((unsigned char)*p)) {
    char *end;
    errno = 0;
    long v = strtol(p, &end, 0);
    if (isalnum((unsigned char)*end) || *end == '_' || errno)
      arith_error("bad number");
    a->p = end;
    return v;
  }
  size_t n = name_len(p);
  if (n) {
    char name[256];
    if (n >= sizeof(name)) arith_error("name too long");
    memcpy(name, p, n);
    name[n] = '\0';
    a->p += n;
    return var_value(name);
  }
  if (!*p) arith_error("missing operand");
  arith_error("syntax error");
}

static long unary(struct ap *a) {
  stack_check(); /* every nested operator and ( comes through here */
  skipws(a);
  char c = *a->p;
  if (c == '+' || c == '-' || c == '!' || c == '~') {
    a->p++;
    long v = unary(a);
    switch (c) {
      case '+':
        return v;
      case '-':
        return wneg(v);
      case '!':
        return !v;
      default:
        return ~v;
    }
  }
  return primary(a);
}

/* Binary operators by precedence level, highest first. */
static int match_op(struct ap *a, const char *op) {
  skipws(a);
  size_t n = strlen(op);
  if (strncmp(a->p, op, n) != 0) return 0;
  /* don't mistake a prefix of a longer operator or an assignment */
  char next = a->p[n];
  int exact = strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
              strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0 ||
              strcmp(op, "&&") == 0 || strcmp(op, "||") == 0;
  if (!exact) {
    if (next == '=') return 0;
    if (n == 1 && next == op[0] && strchr("<>&|", op[0])) return 0;
  }
  a->p += n;
  return 1;
}

static long mul(struct ap *a) {
  long v = unary(a);
  for (;;) {
    if (match_op(a, "*")) {
      v = wmul(v, unary(a));
    } else if (match_op(a, "/") || match_op(a, "%")) {
      char op = a->p[-1];
      long r = unary(a);
      if (r == 0) {
        if (a->noeval) continue;
        arith_error("division by zero");
      }
      if (r == -1) v = op == '/' ? wneg(v) : 0; /* avoid LONG_MIN / -1 trap */
      else v = op == '/' ? v / r : v % r;
    } else {
      return v;
    }
  }
}

static long add(struct ap *a) {
  long v = mul(a);
  for (;;) {
    if (match_op(a, "+"))
      v = wadd(v, mul(a));
    else if (match_op(a, "-"))
      v = wsub(v, mul(a));
    else
      return v;
  }
}

static long shift(struct ap *a) {
  long v = add(a);
  for (;;) {
    if (match_op(a, "<<"))
      v = (long)((unsigned long)v << (add(a) & 63));
    else if (match_op(a, ">>"))
      v >>= (add(a) & 63);
    else
      return v;
  }
}

static long rel(struct ap *a) {
  long v = shift(a);
  for (;;) {
    if (match_op(a, "<="))
      v = v <= shift(a);
    else if (match_op(a, ">="))
      v = v >= shift(a);
    else if (match_op(a, "<"))
      v = v < shift(a);
    else if (match_op(a, ">"))
      v = v > shift(a);
    else
      return v;
  }
}

static long eq(struct ap *a) {
  long v = rel(a);
  for (;;) {
    if (match_op(a, "=="))
      v = v == rel(a);
    else if (match_op(a, "!="))
      v = v != rel(a);
    else
      return v;
  }
}

static long band(struct ap *a) {
  long v = eq(a);
  while (match_op(a, "&")) v &= eq(a);
  return v;
}

static long bxor(struct ap *a) {
  long v = band(a);
  while (match_op(a, "^")) v ^= band(a);
  return v;
}

static long bor(struct ap *a) {
  long v = bxor(a);
  while (match_op(a, "|")) v |= bxor(a);
  return v;
}

static long land(struct ap *a) {
  long v = bor(a);
  while (match_op(a, "&&")) {
    if (!v) a->noeval++;
    long r = bor(a);
    if (!v) a->noeval--;
    v = v && r;
  }
  return v;
}

static long lor(struct ap *a) {
  long v = land(a);
  while (match_op(a, "||")) {
    if (v) a->noeval++;
    long r = land(a);
    if (v) a->noeval--;
    v = v || r;
  }
  return v;
}

static long ternary(struct ap *a) {
  long c = lor(a);
  skipws(a);
  if (*a->p != '?') return c;
  a->p++;
  if (!c) a->noeval++;
  long t = expr_assign(a);
  if (!c) a->noeval--;
  skipws(a);
  if (*a->p != ':') arith_error("expecting ':'");
  a->p++;
  if (c) a->noeval++;
  long f = ternary(a);
  if (c) a->noeval--;
  return c ? t : f;
}

static long expr_assign(struct ap *a) {
  skipws(a);
  const char *save = a->p;
  size_t n = name_len(a->p);
  if (n) {
    char name[256];
    if (n >= sizeof(name)) arith_error("name too long");
    memcpy(name, a->p, n);
    name[n] = '\0';
    a->p += n;
    skipws(a);
    static const char *const ops[] = {"<<=", ">>=", "*=", "/=", "%=", "+=",
                                      "-=",  "&=",  "^=", "|=", "=",  NULL};
    for (int i = 0; ops[i]; i++) {
      size_t ol = strlen(ops[i]);
      if (strncmp(a->p, ops[i], ol) != 0) continue;
      if (ol == 1 && a->p[1] == '=') break; /* == */
      a->p += ol;
      long r = expr_assign(a);
      long v = ol == 1 ? r : var_value(name);
      switch (ops[i][0]) {
        case '<':
          v = (long)((unsigned long)v << (r & 63));
          break;
        case '>':
          v >>= (r & 63);
          break;
        case '*':
          v = wmul(v, r);
          break;
        case '/':
        case '%':
          if (r == 0) {
            if (a->noeval) break;
            arith_error("division by zero");
          }
          v = ops[i][0] == '/' ? (r == -1 ? wneg(v) : v / r) : (r == -1 ? 0 : v % r);
          break;
        case '+':
          v = wadd(v, r);
          break;
        case '-':
          v = wsub(v, r);
          break;
        case '&':
          v &= r;
          break;
        case '^':
          v ^= r;
          break;
        case '|':
          v |= r;
          break;
        default:
          v = r;
      }
      if (!a->noeval) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%ld", v);
        if (var_set(name, buf, 0) < 0) raise_exception(EX_ERROR);
      }
      return v;
    }
    a->p = save;
  }
  return ternary(a);
}

long arith_eval(const char *expr) {
  struct ap a = {expr, 0};
  skipws(&a);
  if (!*a.p) return 0;
  long v = expr_assign(&a);
  skipws(&a);
  if (*a.p) arith_error("syntax error");
  return v;
}
