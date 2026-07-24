/*
 * lr_regex.h - Minimal built-in POSIX-compatible regular expression engine.
 *
 * This is a small, self-contained implementation used on platforms that lack
 * a system <regex.h> (notably MSVC). It implements the subset of the POSIX
 * regex API consumed by L/R_JS:
 *
 *   regcomp, regexec, regfree, regerror
 *   regex_t, regmatch_t (rm_so / rm_eo)
 *   REG_EXTENDED, REG_ICASE, REG_NEWLINE, REG_NOSUB
 *   REG_NOTBOL, REG_NOTEOL, REG_NOMATCH (+ error codes)
 *
 * Supported syntax (Extended Regular Expressions):
 *   literals, . ^ $ \b \B
 *   [...] / [^...] classes, ranges, \d \w \s \D \W \S
 *   grouping (...) and non-capturing (?:...)
 *   alternation |
 *   quantifiers * + ? {n} {n,} {n,m} with lazy (?) variants
 *   escapes \n \t \r \f \v \0 \xHH \uHHHH and backslash-escaped metachars
 *
 * This file is only included on platforms without a real <regex.h> (MSVC),
 * so it does not conflict with the system header elsewhere.
 */
#ifndef LR_REGEX_H
#define LR_REGEX_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Compilation flags (cflags) ────────────────────────────────────────── */
#define REG_EXTENDED  1   /* accepted for POSIX compatibility (no effect) */
#define REG_ICASE     2   /* case-insensitive matching */
#define REG_NEWLINE   4   /* treat newline specially for . ^ $ */
#define REG_NOSUB     8   /* no subexpression capture (accepted, ignored) */

/* ── Execution flags (eflags) ──────────────────────────────────────────── */
#define REG_NOTBOL    16  /* start of string is not beginning of a line */
#define REG_NOTEOL    32  /* end of string is not end of a line */

/* ── Error codes ───────────────────────────────────────────────────────── */
#define REG_NOMATCH   1
#define REG_BADPAT    2
#define REG_ECOLLATE  3
#define REG_ECTYPE    4
#define REG_EESCAPE   5
#define REG_ESUBREG   6
#define REG_EBRACK    7
#define REG_EPAREN    8
#define REG_EBRACE    9
#define REG_BADBR     10
#define REG_ERANGE    11
#define REG_ESPACE    12
#define REG_BADRPT    13

typedef int regoff_t;

typedef struct {
    regoff_t rm_so;   /* start offset of substring (REG_NOMATCH = -1) */
    regoff_t rm_eo;   /* end offset (exclusive) */
} regmatch_t;

/* Opaque compiled-program handle */
typedef struct lr_regex_compiled lr_regex_compiled_t;

typedef struct {
    lr_regex_compiled_t *re_internal;  /* compiled program (NULL if unused) */
    size_t               re_nsub;      /* number of parenthesized subexpressions */
} regex_t;

/*
 * Compile 'regex' according to 'cflags'.
 * Returns 0 on success, or a REG_* error code on failure.
 * On success, 'preg' owns the compiled program and must be freed with regfree().
 */
int  regcomp(regex_t *preg, const char *regex, int cflags);

/*
 * Match the compiled regex against 'string'.
 * 'pmatch' receives up to 'nmatch' submatch offsets (group 0 = whole match).
 * Returns 0 on match, REG_NOMATCH if no match.
 */
int  regexec(const regex_t *preg, const char *string,
             size_t nmatch, regmatch_t pmatch[], int eflags);

/* Free resources owned by 'preg'. */
void regfree(regex_t *preg);

/* Format an error message into 'errbuf' (up to 'errbuf_size' bytes).
 * Returns the length of the full message (excluding terminator). */
size_t regerror(int errcode, const regex_t *preg,
                char *errbuf, size_t errbuf_size);

#ifdef __cplusplus
}
#endif

#endif /* LR_REGEX_H */
