/*
 * lr_regex.c - Minimal built-in POSIX-compatible regular expression engine.
 *
 * See lr_regex.h for the supported API and syntax. This is a compact,
 * self-contained backtracking matcher written for L/R_JS so the project can
 * be built with MSVC, which does not ship <regex.h>.
 *
 * It favours correctness and small size over pathological-case performance;
 * a global step budget bounds backtracking so degenerate patterns cannot
 * hang the engine.
 */
#include "lr_regex.h"

#include <stdlib.h>
#include <string.h>

/* ── Limits ─────────────────────────────────────────────────────────────── */
#define LR_RE_MAX_STEPS  2000000   /* backtracking step budget per exec */
#define LR_RE_MAX_REPEAT 100000    /* cap on {n,m} repetition counts */

/* ── AST node types ─────────────────────────────────────────────────────── */
#define T_CHAR  1
#define T_ANY   2
#define T_CLASS 3
#define T_ANCH  4   /* anchor: 0='^' 1='$' 2='\b' 3='\B' */
#define T_GROUP 5   /* capturing or non-capturing */
#define T_CAT   6   /* concatenation */
#define T_ALT   7   /* alternation */
#define T_REP   8   /* repetition (quantifier) */

typedef struct Node Node;
struct Node {
    int type;
    int ch;            /* T_CHAR */
    unsigned char cls[32]; /* T_CLASS: 256-bit membership bitmap */
    int neg;           /* T_CLASS: negate the bitmap */
    int anchor;        /* T_ANCH */
    int cap;           /* T_GROUP: 1-based capture index, 0 = non-capturing */
    Node *child;       /* T_GROUP, T_REP */
    Node **items;      /* T_CAT, T_ALT */
    int nitems;        /* T_CAT, T_ALT */
    int min, max;      /* T_REP: max < 0 means unbounded */
    int greedy;        /* T_REP: 1 = greedy, 0 = lazy */
};

/* ── Compiled program ───────────────────────────────────────────────────── */
typedef struct lr_regex_compiled {
    Node *root;
    int   ngroups;
    int   flags;
} Compiled;

/* ── Parser context ─────────────────────────────────────────────────────── */
typedef struct {
    const char *p;
    const char *end;
    int         flags;
    int         ngroup;
    int         error;
} PCtx;
typedef PCtx *PCtx_P;

/* ── Character-class bitmap helpers (operate on 256 bits) ───────────────── */
static void cls_set(unsigned char *c, int b)
{
    if (b >= 0 && b < 256) c[b >> 3] |= (unsigned char)(1u << (b & 7));
}
static int cls_has(unsigned char *c, int b)
{
    if (b < 0 || b >= 256) return 0;
    return (c[b >> 3] >> (b & 7)) & 1;
}
static void cls_set_range(unsigned char *c, int lo, int hi)
{
    for (int x = lo; x <= hi; x++) cls_set(c, x);
}
static void cls_set_all(unsigned char *c)
{
    for (int i = 0; i < 32; i++) c[i] = 0xFF;
}
static void cls_clear_range(unsigned char *c, int lo, int hi)
{
    for (int x = lo; x <= hi; x++)
        if (x >= 0 && x < 256) c[x >> 3] &= (unsigned char)~(1u << (x & 7));
}

static int lr_isword(int ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '_';
}
static void cls_set_word(unsigned char *c)
{
    cls_set_range(c, 'a', 'z');
    cls_set_range(c, 'A', 'Z');
    cls_set_range(c, '0', '9');
    cls_set(c, '_');
}
static void cls_set_space(unsigned char *c)
{
    cls_set(c, ' ');
    cls_set(c, '\t');
    cls_set(c, '\n');
    cls_set(c, '\r');
    cls_set(c, '\f');
    cls_set(c, '\v');
}
static void cls_clear_word(unsigned char *c)
{
    cls_clear_range(c, 'a', 'z');
    cls_clear_range(c, 'A', 'Z');
    cls_clear_range(c, '0', '9');
    cls_clear_range(c, '_', '_');
}
static void cls_clear_space(unsigned char *c)
{
    cls_clear_range(c, ' ', ' ');
    cls_clear_range(c, '\t', '\t');
    cls_clear_range(c, '\n', '\n');
    cls_clear_range(c, '\r', '\r');
    cls_clear_range(c, '\f', '\f');
    cls_clear_range(c, '\v', '\v');
}

static int lr_tolower(int c)
{
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}
static int lr_toupper(int c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    return c;
}

/* ── Hex digit parsing ──────────────────────────────────────────────────── */
static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int hex2(PCtx_P c)
{
    if (c->p + 1 >= c->end) return -1;
    int h = hexval((unsigned char)c->p[0]);
    int l = hexval((unsigned char)c->p[1]);
    if (h < 0 || l < 0) return -1;
    c->p += 2;
    return h * 16 + l;
}
static int hex4(PCtx_P c)
{
    if (c->p + 3 >= c->end) return -1;
    int v = 0;
    for (int i = 0; i < 4; i++) {
        int d = hexval((unsigned char)c->p[i]);
        if (d < 0) return -1;
        v = v * 16 + d;
    }
    c->p += 4;
    return v;
}

/* ── AST allocation / freeing ───────────────────────────────────────────── */
static Node *new_node(int type)
{
    Node *n = (Node *)calloc(1, sizeof(Node));
    if (n) n->type = type;
    return n;
}
static void node_free(Node *n)
{
    if (!n) return;
    switch (n->type) {
    case T_GROUP:
    case T_REP:
        node_free(n->child);
        break;
    case T_ALT:
    case T_CAT:
        for (int i = 0; i < n->nitems; i++) node_free(n->items[i]);
        free(n->items);
        break;
    default:
        break;
    }
    free(n);
}

/* ── Parser ─────────────────────────────────────────────────────────────── */
static Node *parse_alt(PCtx *c);

/* Parse an escape inside a character class.
 * Returns a literal character (>= 0), or -1 if a class shorthand was added
 * directly to 'cls' (and no single literal applies). */
static int class_escape(PCtx *c, unsigned char *cls)
{
    int e = (unsigned char)*c->p;
    c->p++;
    switch (e) {
    case 'd': cls_set_range(cls, '0', '9'); return -1;
    case 'D': cls_set_all(cls); cls_clear_range(cls, '0', '9'); return -1;
    case 'w': cls_set_word(cls); return -1;
    case 'W': cls_set_all(cls); cls_clear_word(cls); return -1;
    case 's': cls_set_space(cls); return -1;
    case 'S': cls_set_all(cls); cls_clear_space(cls); return -1;
    case 'b': return 0x08;          /* backspace inside a class */
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'f': return '\f';
    case 'v': return '\v';
    case '0': return 0;
    case 'x': {
        int v = hex2(c);
        if (v < 0) { c->error = REG_EESCAPE; return -1; }
        return v;
    }
    case 'u': {
        int v = hex4(c);
        if (v < 0) { c->error = REG_EESCAPE; return -1; }
        return v;
    }
    default:  return e;             /* escaped literal metachar */
    }
}

/* Attempt to parse a {n}, {n,}, or {n,m} quantifier starting at '*c->p'.
 * On success advances c->p past '}' and returns 1; otherwise leaves c->p
 * unchanged and returns 0 (so '{' is treated as a literal later). */
static int parse_brace(PCtx *c, int *min, int *max, int *greedy)
{
    const char *q = c->p + 1;
    int lo = 0, hi = -1;

    if (q >= c->end || !(*q >= '0' && *q <= '9')) return 0;
    lo = 0;
    while (q < c->end && *q >= '0' && *q <= '9') {
        lo = lo * 10 + (*q - '0');
        if (lo > LR_RE_MAX_REPEAT) lo = LR_RE_MAX_REPEAT;
        q++;
    }
    if (q < c->end && *q == ',') {
        q++;
        if (q < c->end && *q >= '0' && *q <= '9') {
            hi = 0;
            while (q < c->end && *q >= '0' && *q <= '9') {
                hi = hi * 10 + (*q - '0');
                if (hi > LR_RE_MAX_REPEAT) hi = LR_RE_MAX_REPEAT;
                q++;
            }
        } else {
            hi = -1;
        }
    } else {
        hi = lo;
    }
    if (q >= c->end || *q != '}') return 0;

    c->p = q + 1;
    *min = lo;
    *max = hi;
    *greedy = 1;
    return 1;
}

/* Parse a [...] character class. */
static Node *parse_class(PCtx *c)
{
    c->p++;  /* consume '[' */
    int neg = 0;
    if (c->p < c->end && *c->p == '^') { neg = 1; c->p++; }

    unsigned char cls[32];
    memset(cls, 0, sizeof(cls));
    int first = 1;

    while (c->p < c->end) {
        int ch = (unsigned char)*c->p;
        if (ch == ']' && !first) {
            c->p++;  /* consume ']' */
            Node *n = new_node(T_CLASS);
            if (!n) { c->error = REG_ESPACE; return NULL; }
            memcpy(n->cls, cls, sizeof(cls));
            n->neg = neg;
            return n;
        }
        first = 0;

        int lo;
        if (ch == '\\') {
            c->p++;
            if (c->p >= c->end) { c->error = REG_EESCAPE; return NULL; }
            int lit = class_escape(c, cls);
            if (lit < 0) {
                if (c->error) return NULL;
                continue;  /* shorthand class added to bitmap */
            }
            lo = lit;
        } else {
            lo = ch;
            c->p++;
        }

        /* Range?  lo '-' hi */
        if (c->p < c->end && *c->p == '-' &&
            c->p + 1 < c->end && c->p[1] != ']') {
            c->p++;  /* consume '-' */
            int hi;
            if ((unsigned char)*c->p == '\\') {
                c->p++;
                if (c->p >= c->end) { c->error = REG_EESCAPE; return NULL; }
                int lit = class_escape(c, cls);
                if (lit < 0) { c->error = REG_ECTYPE; return NULL; }
                hi = lit;
            } else {
                hi = (unsigned char)*c->p;
                c->p++;
            }
            if (lo > hi) { c->error = REG_ERANGE; return NULL; }
            cls_set_range(cls, lo, hi);
        } else {
            cls_set(cls, lo);
        }
    }

    c->error = REG_EBRACK;
    return NULL;
}

/* Parse an escape sequence outside a class, returning a node. */
static Node *parse_escape(PCtx *c)
{
    c->p++;  /* consume backslash */
    if (c->p >= c->end) { c->error = REG_EESCAPE; return NULL; }
    int e = (unsigned char)*c->p;
    c->p++;

    Node *n = new_node(T_CHAR);
    if (!n) { c->error = REG_ESPACE; return NULL; }

    switch (e) {
    case 'b': n->type = T_ANCH; n->anchor = 2; return n;
    case 'B': n->type = T_ANCH; n->anchor = 3; return n;
    case 'd': n->type = T_CLASS; cls_set_range(n->cls, '0', '9'); return n;
    case 'D': n->type = T_CLASS; cls_set_all(n->cls); cls_clear_range(n->cls, '0', '9'); return n;
    case 'w': n->type = T_CLASS; cls_set_word(n->cls); return n;
    case 'W': n->type = T_CLASS; cls_set_all(n->cls); cls_clear_word(n->cls); return n;
    case 's': n->type = T_CLASS; cls_set_space(n->cls); return n;
    case 'S': n->type = T_CLASS; cls_set_all(n->cls); cls_clear_space(n->cls); return n;
    case 'n': n->ch = '\n'; return n;
    case 't': n->ch = '\t'; return n;
    case 'r': n->ch = '\r'; return n;
    case 'f': n->ch = '\f'; return n;
    case 'v': n->ch = '\v'; return n;
    case '0': n->ch = 0; return n;
    case 'x': {
        int v = hex2(c);
        if (v < 0) { node_free(n); c->error = REG_EESCAPE; return NULL; }
        n->ch = v; return n;
    }
    case 'u': {
        int v = hex4(c);
        if (v < 0) { node_free(n); c->error = REG_EESCAPE; return NULL; }
        n->ch = v; return n;
    }
    default:  n->ch = e; return n;  /* escaped literal */
    }
}

/* Parse a single atom. */
static Node *parse_atom(PCtx *c)
{
    if (c->p >= c->end) { c->error = REG_BADPAT; return NULL; }
    int ch = (unsigned char)*c->p;

    if (ch == '(') {
        c->p++;
        int capturing = 1;
        int group_index = 0;

        if (c->p < c->end && *c->p == '?') {
            if (c->p + 1 < c->end && c->p[1] == ':') {
                capturing = 0;
                c->p += 2;
            } else {
                /* (?<name>...), (?=...), (?!...), (?<=...), (?<!...) etc.
                 * We treat these as non-capturing (POSIX has no lookaround). */
                capturing = 0;
                c->p++;  /* skip '?' */
                while (c->p < c->end && *c->p != ':' && *c->p != ')') c->p++;
                if (c->p < c->end && *c->p == ':') c->p++;
            }
        }
        if (capturing) {
            c->ngroup++;
            group_index = c->ngroup;
        }

        Node *inner = parse_alt(c);
        if (!inner) return NULL;
        if (c->p >= c->end || *c->p != ')') {
            node_free(inner);
            c->error = REG_EPAREN;
            return NULL;
        }
        c->p++;  /* consume ')' */

        Node *g = new_node(T_GROUP);
        if (!g) { node_free(inner); c->error = REG_ESPACE; return NULL; }
        g->cap = capturing ? group_index : 0;
        g->child = inner;
        return g;
    }
    if (ch == '[') return parse_class(c);
    if (ch == '.') { c->p++; return new_node(T_ANY); }
    if (ch == '^') { c->p++; Node *n = new_node(T_ANCH); n->anchor = 0; return n; }
    if (ch == '$') { c->p++; Node *n = new_node(T_ANCH); n->anchor = 1; return n; }
    if (ch == '\\') return parse_escape(c);

    /* literal character */
    c->p++;
    Node *n = new_node(T_CHAR);
    if (!n) { c->error = REG_ESPACE; return NULL; }
    n->ch = ch;
    return n;
}

/* Parse an atom followed by optional quantifiers (which may repeat). */
static Node *parse_rep(PCtx *c)
{
    Node *atom = parse_atom(c);
    if (!atom) return NULL;

    for (;;) {
        if (c->p >= c->end) break;
        int ch = (unsigned char)*c->p;
        int min = 0, max = 0, greedy = 1, isrep = 0;

        if (ch == '*')      { min = 0; max = -1; isrep = 1; }
        else if (ch == '+') { min = 1; max = -1; isrep = 1; }
        else if (ch == '?') { min = 0; max = 1;  isrep = 1; }
        else if (ch == '{') {
            if (!parse_brace(c, &min, &max, &greedy)) break;  /* literal '{' */
            isrep = 1;
        }
        if (!isrep) break;

        c->p++;  /* consume the quantifier character */
        if (c->p < c->end && *c->p == '?') { greedy = 0; c->p++; }

        Node *rep = new_node(T_REP);
        if (!rep) { node_free(atom); c->error = REG_ESPACE; return NULL; }
        rep->child = atom;
        rep->min = min;
        rep->max = max;
        rep->greedy = greedy;
        atom = rep;
    }
    return atom;
}

/* Parse a concatenation (sequence of quantified atoms). */
static Node *parse_cat(PCtx *c)
{
    Node **items = NULL;
    int nitems = 0, cap = 0;
    int rc = 0;

    while (c->p < c->end) {
        int ch = (unsigned char)*c->p;
        if (ch == '|' || ch == ')') break;
        Node *a = parse_rep(c);
        if (!a) { rc = c->error ? c->error : REG_BADPAT; goto fail; }
        if (nitems == cap) {
            int nc = cap ? cap * 2 : 4;
            Node **na = (Node **)realloc(items, (size_t)nc * sizeof(Node *));
            if (!na) { node_free(a); rc = REG_ESPACE; goto fail; }
            items = na; cap = nc;
        }
        items[nitems++] = a;
    }

    if (nitems == 0) {
        Node *n = new_node(T_CAT);
        if (!n) { rc = REG_ESPACE; goto fail; }
        n->items = NULL; n->nitems = 0;
        return n;
    }
    if (nitems == 1) {
        Node *r = items[0];
        free(items);
        return r;
    }
    Node *n = new_node(T_CAT);
    if (!n) { rc = REG_ESPACE; goto fail; }
    n->items = items; n->nitems = nitems;
    return n;

fail:
    for (int i = 0; i < nitems; i++) node_free(items[i]);
    free(items);
    c->error = rc;
    return NULL;
}

/* Parse a full alternation: cat ('|' cat)* */
static Node *parse_alt(PCtx *c)
{
    Node **items = NULL;
    int nitems = 0, cap = 0;
    int rc = 0;

    Node *first = parse_cat(c);
    if (!first) return NULL;
    if (nitems == cap) {
        int nc = cap ? cap * 2 : 4;
        Node **na = (Node **)realloc(items, (size_t)nc * sizeof(Node *));
        if (!na) { node_free(first); return NULL; }
        items = na; cap = nc;
    }
    items[nitems++] = first;

    while (c->p < c->end && *c->p == '|') {
        c->p++;  /* consume '|' */
        Node *a = parse_cat(c);
        if (!a) { rc = c->error ? c->error : REG_BADPAT; goto fail; }
        if (nitems == cap) {
            int nc = cap ? cap * 2 : 4;
            Node **na = (Node **)realloc(items, (size_t)nc * sizeof(Node *));
            if (!na) { node_free(a); rc = REG_ESPACE; goto fail; }
            items = na; cap = nc;
        }
        items[nitems++] = a;
    }

    if (nitems == 1) {
        free(items);
        return first;
    }
    Node *n = new_node(T_ALT);
    if (!n) { rc = REG_ESPACE; goto fail; }
    n->items = items; n->nitems = nitems;
    return n;

fail:
    for (int i = 0; i < nitems; i++) node_free(items[i]);
    free(items);
    c->error = rc;
    return NULL;
}

/* ── Matcher ────────────────────────────────────────────────────────────── */
typedef struct {
    const unsigned char *s;
    int   len;
    int  *cap;        /* 2*(ngroups+1) capture slots: [so, eo] per group */
    int   capcount;
    int   steps;
    int   eflags;
    int   flags;
} Mctx;

static int class_match(Node *n, int sc, int flags)
{
    int matched = cls_has(n->cls, sc);
    if ((flags & REG_ICASE) && !matched) {
        matched = cls_has(n->cls, lr_tolower(sc)) ||
                  cls_has(n->cls, lr_toupper(sc));
    }
    return n->neg ? !matched : matched;
}

static int anchor_ok(int anchor, const unsigned char *s, int len, int pos,
                      int eflags, int flags)
{
    (void)eflags;
    if (anchor == 0) {  /* ^ */
        if (pos == 0) return 1;
        if ((flags & REG_NEWLINE) && pos > 0 && s[pos - 1] == '\n') return 1;
        return 0;
    }
    if (anchor == 1) {  /* $ */
        if (pos == len) return 1;
        if ((flags & REG_NEWLINE) && pos < len && s[pos] == '\n') return 1;
        return 0;
    }
    if (anchor == 2) {  /* \b */
        int before = (pos > 0)      ? lr_isword(s[pos - 1]) : 0;
        int after  = (pos < len)    ? lr_isword(s[pos])     : 0;
        return before != after;
    }
    if (anchor == 3) {  /* \B */
        int before = (pos > 0)      ? lr_isword(s[pos - 1]) : 0;
        int after  = (pos < len)    ? lr_isword(s[pos])     : 0;
        return before == after;
    }
    return 0;
}

/* ── Continuation-passing matcher ──────────────────────────────────────── */
/* The matcher threads a "continuation" (what to do after a node matches),
 * which lets quantifiers and alternations backtrack correctly: when the
 * continuation fails, the node simply tries the next alternative. This is
 * essential for patterns like "a.*c" where a greedy sub-expression must give
 * up characters so that the rest of the pattern can match. */

typedef struct Cont Cont;
struct Cont {
    int  (*fn)(Mctx *m, int pos, Cont *self);
    Cont *parent;     /* continuation to run after this one completes */
    Node **items;     /* sequence (T_CAT) continuation */
    int   idx;        /* current index into items */
    int   n;          /* number of items */
    Node *node;       /* child node (repetition step) */
    int   count;      /* remaining repetitions (repetition step) */
    int   group;      /* group index (group-end continuation) */
};

/* Run a continuation; NULL means "the whole pattern has matched". */
static int m_invoke(Mctx *m, int pos, Cont *k)
{
    if (!k) return pos;
    return k->fn(m, pos, k);
}

/* Trivial continuation that just echoes the position. */
static int stop_fn(Mctx *m, int pos, Cont *self)
{
    (void)m; (void)self;
    return pos;
}
static Cont STOP_CONT = { stop_fn, NULL, NULL, 0, 0, NULL, 0, 0 };

static int m_node(Mctx *m, Node *n, int pos, Cont *k);

/* Sequence continuation: after items[0..idx-1] matched at 'pos', continue
 * with items[idx..]; when exhausted, run the parent continuation. */
static int seq_step(Mctx *m, int pos, Cont *self)
{
    if (self->idx >= self->n) return m_invoke(m, pos, self->parent);
    Cont c;
    c.fn = seq_step; c.parent = self->parent;
    c.items = self->items; c.idx = self->idx + 1; c.n = self->n;
    c.node = NULL; c.count = 0; c.group = 0;
    return m_node(m, self->items[self->idx], pos, &c);
}

/* Group-end continuation: record the end offset then continue. */
static int group_end(Mctx *m, int pos, Cont *self)
{
    if (self->group > 0) m->cap[2 * self->group + 1] = pos;
    return m_invoke(m, pos, self->parent);
}

/* Repetition: match 'child' exactly 'count' times, then run k. */
static int rep_step(Mctx *m, int pos, Cont *self);
static int rep_match(Mctx *m, Node *child, int count, int pos, Cont *k)
{
    if (count <= 0) return m_invoke(m, pos, k);
    Cont c;
    c.fn = rep_step; c.parent = k;
    c.items = NULL; c.idx = 0; c.n = 0;
    c.node = child; c.count = count - 1; c.group = 0;
    return m_node(m, child, pos, &c);
}
static int rep_step(Mctx *m, int pos, Cont *self)
{
    return rep_match(m, self->node, self->count, pos, self->parent);
}

/* Count how many times 'child' can match (requiring forward progress except
 * for a single zero-width match) starting at 'pos'. */
static int count_max_reps(Mctx *m, Node *child, int pos)
{
    int cnt = 0;
    int p = pos;
    for (;;) {
        int np = m_node(m, child, p, &STOP_CONT);
        if (np < 0) break;
        cnt++;
        if (np == p) break;            /* zero-width: count once, stop */
        p = np;
        if (cnt > LR_RE_MAX_REPEAT) break;
    }
    return cnt;
}

static int m_node(Mctx *m, Node *n, int pos, Cont *k)
{
    if (++m->steps > LR_RE_MAX_STEPS) return -1;

    switch (n->type) {
    case T_CHAR: {
        if (pos < m->len) {
            int sc = m->s[pos];
            if (sc == n->ch ||
                ((m->flags & REG_ICASE) && lr_tolower(sc) == lr_tolower(n->ch)))
                return m_invoke(m, pos + 1, k);
        }
        return -1;
    }
    case T_ANY: {
        if (pos < m->len) {
            if ((m->flags & REG_NEWLINE) && m->s[pos] == '\n') return -1;
            return m_invoke(m, pos + 1, k);
        }
        return -1;
    }
    case T_CLASS: {
        if (pos < m->len && class_match(n, m->s[pos], m->flags))
            return m_invoke(m, pos + 1, k);
        return -1;
    }
    case T_ANCH: {
        return anchor_ok(n->anchor, m->s, m->len, pos, m->eflags, m->flags)
                   ? m_invoke(m, pos, k) : -1;
    }
    case T_GROUP: {
        int g = n->cap;
        int os = -1, oe = -1;
        if (g > 0) {
            os = m->cap[2 * g];
            oe = m->cap[2 * g + 1];
            m->cap[2 * g] = pos;
            m->cap[2 * g + 1] = -1;
        }
        Cont c;
        c.fn = group_end; c.parent = k;
        c.items = NULL; c.idx = 0; c.n = 0; c.node = NULL; c.count = 0;
        c.group = g;
        int e = m_node(m, n->child, pos, &c);
        if (e < 0 && g > 0) {
            m->cap[2 * g] = os;
            m->cap[2 * g + 1] = oe;
        }
        return e;
    }
    case T_CAT: {
        if (n->nitems == 0) return m_invoke(m, pos, k);
        Cont c;
        c.fn = seq_step; c.parent = k;
        c.items = n->items; c.idx = 1; c.n = n->nitems;
        c.node = NULL; c.count = 0; c.group = 0;
        return m_node(m, n->items[0], pos, &c);
    }
    case T_ALT: {
        int *saved = (int *)malloc((size_t)m->capcount * sizeof(int));
        if (saved) memcpy(saved, m->cap, (size_t)m->capcount * sizeof(int));
        for (int i = 0; i < n->nitems; i++) {
            if (saved) memcpy(m->cap, saved, (size_t)m->capcount * sizeof(int));
            int e = m_node(m, n->items[i], pos, k);
            if (e >= 0) { free(saved); return e; }
        }
        if (saved) memcpy(m->cap, saved, (size_t)m->capcount * sizeof(int));
        free(saved);
        return -1;
    }
    case T_REP: {
        /* Protect captures while we probe how many reps are possible. */
        int *saved = (int *)malloc((size_t)m->capcount * sizeof(int));
        if (saved) memcpy(saved, m->cap, (size_t)m->capcount * sizeof(int));
        int cnt = count_max_reps(m, n->child, pos);
        if (saved) memcpy(m->cap, saved, (size_t)m->capcount * sizeof(int));
        free(saved);

        int hi = (n->max < 0) ? cnt : (cnt < n->max ? cnt : n->max);
        if (hi < n->min) return -1;

        if (n->greedy) {
            for (int c = hi; c >= n->min; c--) {
                int e = rep_match(m, n->child, c, pos, k);
                if (e >= 0) return e;
            }
        } else {
            for (int c = n->min; c <= hi; c++) {
                int e = rep_match(m, n->child, c, pos, k);
                if (e >= 0) return e;
            }
        }
        return -1;
    }
    default:
        return -1;
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */
int regcomp(regex_t *preg, const char *regex, int cflags)
{
    if (!preg) return REG_ESPACE;
    preg->re_internal = NULL;
    preg->re_nsub = 0;
    if (!regex) return REG_BADPAT;

    PCtx c;
    c.p = regex;
    c.end = regex + strlen(regex);
    c.flags = cflags;
    c.ngroup = 0;
    c.error = 0;

    Node *root = parse_alt(&c);
    if (!root) return c.error ? c.error : REG_BADPAT;
    if (c.p < c.end) {
        int err = (*c.p == ')') ? REG_EPAREN : (c.error ? c.error : REG_BADPAT);
        node_free(root);
        return err;
    }

    Compiled *comp = (Compiled *)malloc(sizeof(Compiled));
    if (!comp) { node_free(root); return REG_ESPACE; }
    comp->root = root;
    comp->ngroups = c.ngroup;
    comp->flags = cflags;

    preg->re_internal = comp;
    preg->re_nsub = (size_t)c.ngroup;
    return 0;
}

int regexec(const regex_t *preg, const char *string,
            size_t nmatch, regmatch_t pmatch[], int eflags)
{
    if (!preg || !preg->re_internal) return REG_NOMATCH;
    Compiled *comp = (Compiled *)preg->re_internal;

    int len = (int)strlen(string ? string : "");
    int ng = comp->ngroups;
    int capcount = 2 * (ng + 1);

    int *cap = (int *)malloc((size_t)capcount * sizeof(int));
    if (!cap) return REG_ESPACE;

    Mctx m;
    m.s = (const unsigned char *)(string ? string : "");
    m.len = len;
    m.cap = cap;
    m.capcount = capcount;
    m.steps = 0;
    m.eflags = eflags;
    m.flags = comp->flags;

    int found = -1, endpos = -1;
    for (int pos = 0; pos <= len; pos++) {
        for (int i = 0; i < capcount; i++) cap[i] = -1;
        int e = m_node(&m, comp->root, pos, NULL);
        if (e >= 0) { found = pos; endpos = e; break; }
    }

    if (found < 0) { free(cap); return REG_NOMATCH; }

    cap[0] = found;
    cap[1] = endpos;

    if (pmatch && nmatch > 0) {
        size_t gmax = (nmatch < (size_t)(ng + 1)) ? nmatch : (size_t)(ng + 1);
        for (size_t g = 0; g < nmatch; g++) {
            if (g < gmax && cap[2 * (int)g] >= 0) {
                pmatch[g].rm_so = cap[2 * (int)g];
                int eo = cap[2 * (int)g + 1];
                pmatch[g].rm_eo = (eo < 0) ? cap[2 * (int)g] : eo;
            } else {
                pmatch[g].rm_so = -1;
                pmatch[g].rm_eo = -1;
            }
        }
    }

    free(cap);
    return 0;
}

void regfree(regex_t *preg)
{
    if (!preg) return;
    if (preg->re_internal) {
        Compiled *comp = (Compiled *)preg->re_internal;
        node_free(comp->root);
        free(comp);
        preg->re_internal = NULL;
    }
}

size_t regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size)
{
    (void)preg;
    const char *msg;
    switch (errcode) {
    case REG_NOMATCH: msg = "No match"; break;
    case REG_BADPAT:  msg = "Invalid regular expression"; break;
    case REG_EPAREN:  msg = "Unmatched parenthesis"; break;
    case REG_EBRACK:  msg = "Unmatched brackets"; break;
    case REG_EBRACE:  msg = "Unmatched braces"; break;
    case REG_ERANGE:  msg = "Invalid endpoint in range"; break;
    case REG_EESCAPE: msg = "Trailing backslash"; break;
    case REG_ESUBREG: msg = "Invalid backreference"; break;
    case REG_ECTYPE:  msg = "Invalid character class name"; break;
    case REG_ESPACE:  msg = "Out of memory"; break;
    case REG_BADRPT:  msg = "Invalid repetition operator"; break;
    default:          msg = "Unknown regex error"; break;
    }

    size_t n = strlen(msg);
    if (errbuf && errbuf_size > 0) {
        size_t copy = (n < errbuf_size - 1) ? n : (errbuf_size - 1);
        memcpy(errbuf, msg, copy);
        errbuf[copy] = '\0';
    }
    return n + 1;
}
