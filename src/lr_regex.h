/* lr_regex.h - PCRE2-based regular expression engine.
 *
 * This header provides a POSIX-compatible regular expression API (regcomp,
 * regexec, regfree, regerror, regex_t, regmatch_t) built on top of PCRE2.
 * Unlike the system <regex.h> (POSIX), this supports modern Perl-compatible
 * syntax: named capture groups (?<name>...), lookaheads/lookbehinds,
 * possessive quantifiers, and all PCRE2 features.
 *
 * On MSVC (which has no <regex.h>) this file replaces the system header.
 * On Linux/macOS it shadows the POSIX <regex.h> with a PCRE2-based
 * implementation that accepts a much richer syntax.
 *
 * Optimisations (round-21):
 *   - Pattern compilation cache: repeated compilation of the same pattern
 *     with the same flags hits the cache instead of re-running PCRE2.
 *   - Named-group hash map: lr_regex_named_group_index() is O(1).
 *   - JIT acceleration when PCRE2 is built with SUPPORT_JIT.
 *   - ASCII-only patterns skip PCRE2_UCP.
 *
 * Thread safety: regcomp/regexec/regfree are NOT thread-safe for the same
 * regex_t; use one regex_t per thread.
 */
#ifndef LR_REGEX_H
#define LR_REGEX_H

#include <stddef.h>
#include <stdint.h>

/* ── POSIX-compatible constants ────────────────────────────────────────── */

#define REG_EXTENDED  1   /* accepted for compatibility (PCRE2 is always ERE+) */
#define REG_ICASE     2   /* case-insensitive matching → PCRE2_CASELESS */
#define REG_NEWLINE   4   /* ^/$ match at newlines       → PCRE2_MULTILINE */
#define REG_NOSUB     8   /* caller does not need match data (optimisation) */

/* Error codes returned by regcomp / regexec */
#define REG_SUCCESS    0
#define REG_NOMATCH    1   /* regexec: no match */
#define REG_ESPACE     2   /* out of memory */
#define REG_ECOLLATE   3   /* invalid collation element (not used) */
#define REG_ENOSYS     4   /* not implemented (not used) */
#define REG_EBRACK     5   /* unbalanced bracket */
#define REG_EPAREN     6   /* unbalanced parenthesis */
#define REG_EBRACE     7   /* unbalanced brace */
#define REG_BADBR      8   /* invalid repetition count */
#define REG_ERANGE     9   /* invalid character range */
#define REG_BADRPT     10  /* invalid repetition */
#define REG_ECTYPE     11  /* invalid character class */
#define REG_EEOF       12  /* unexpected EOF */
#define REG_EESCAPE    13  /* invalid escape */
#define REG_ESUBREG    14  /* invalid back reference */
#define REG_EBADPAT    15  /* general pattern error */
#define REG_ERPAREN    16  /* unbalanced parentheses */
#define REG_BADPAT     17  /* bad pattern */

/* ── regmatch_t ────────────────────────────────────────────────────────── */
/* POSIX-compatible match offsets. */
typedef struct {
    int rm_so;   /* byte offset of start of match (or -1 if not participating) */
    int rm_eo;   /* byte offset of end of match */
} regmatch_t;

/* ── regex_t ───────────────────────────────────────────────────────────── */
/* Compiled regex handle.  Fields:
 *   re_nsub      – number of capturing subpatterns
 *   __code       – opaque pointer to the compiled PCRE2 code
 *   __mdata      – opaque pointer to the (pre-allocated) match data block
 *   __flags      – stored compilation flags
 *   __errbuf     – last error message (internal, 256 bytes)
 *   __named_map  – opaque pointer to the pre-built named-group hash map
 *                  (LrNamedGroupMap*; NULL when not yet built)
 *
 * NOTE: __code and __mdata are shared with the global compilation cache.
 * regfree() clears them but does NOT free the underlying pcre2_code /
 * pcre2_match_data (those are freed when the cache evicts the entry).
 */
#define LR_REGEX_ERRBUF_SIZE 256

typedef struct {
    size_t  re_nsub;          /* number of capturing subpatterns */
    void   *__code;            /* pcre2_code * (owned by cache) */
    void   *__mdata;           /* pcre2_match_data * (owned by cache) */
    void   *__named_map;       /* LrNamedGroupMap * (freed by regfree) */
    int     __flags;           /* cflags passed to regcomp */
    int     __has_jit;         /* 1 if JIT compilation succeeded for this pattern */
    char    __errbuf[LR_REGEX_ERRBUF_SIZE]; /* last error message */
} regex_t;

/* ── POSIX API ─────────────────────────────────────────────────────────── */

/*
 * regcomp - Compile a regular expression pattern.
 *   preg    : uninitialised regex_t (caller allocates)
 *   regex   : null-terminated pattern string
 *   cflags  : bitwise OR of REG_EXTENDED, REG_ICASE, REG_NEWLINE, REG_NOSUB
 * Returns 0 on success, non-zero on error.
 * On error, preg->__errbuf contains a human-readable message.
 */
int  regcomp(regex_t *preg, const char *regex, int cflags);

/*
 * regexec - Execute a compiled regex against a subject string.
 *   preg    : compiled regex_t (from regcomp)
 *   string  : null-terminated subject string
 *   nmatch  : size of pmatch array
 *   pmatch  : array of regmatch_t to receive match offsets
 *   eflags  : 0 (reserved, ignored)
 * Returns 0 on match, REG_NOMATCH if no match, or an error code.
 * pmatch[0] receives the full match; pmatch[1..n] capture groups.
 * Unmatched groups have rm_so = -1, rm_eo = -1.
 */
int  regexec(const regex_t *preg, const char *string,
             size_t nmatch, regmatch_t pmatch[], int eflags);

/*
 * regfree - Reset a compiled regex_t.
 *
 * Clears the internal pointers so the regex_t struct can be reused.
 * The underlying pcre2_code and pcre2_match_data remain in the global
 * compilation cache (they are freed when the cache evicts them).
 * The named-group hash map (if any) is freed here.
 */
void regfree(regex_t *preg);

/*
 * regerror - Convert an error code to a human-readable string.
 *   errcode    : value returned by regcomp / regexec
 *   preg       : the regex_t that produced the error (may be NULL)
 *   errbuf     : output buffer
 *   errbuf_size: size of output buffer
 * Returns the length of the error string (may be > errbuf_size).
 */
size_t regerror(int errcode, const regex_t *preg,
                char *errbuf, size_t errbuf_size);

/*
 * lr_regex_cache_free_all - Destroy every entry in the global pattern
 *                           compilation cache.  Call at shutdown time
 *                           to avoid leaking pcre2_code blocks.
 */
void lr_regex_cache_free_all(void);

/* ── PCRE2 extensions (optional, for advanced use) ──────────────────────── */

/*
 * lr_regex_named_group_count - Return the number of named capture groups.
 */
int lr_regex_named_group_count(const regex_t *preg);

/*
 * lr_regex_named_group_name - Get the name of a named capture group by index.
 *   preg     : compiled regex_t
 *   idx      : index (0 .. count-1)
 *   name     : output buffer
 *   name_len : size of output buffer
 * Returns 0 on success, -1 if idx is out of range.
 */
int lr_regex_named_group_name(const regex_t *preg, int idx,
                               char *name, size_t name_len);

/*
 * lr_regex_named_group_index - Get the capture group index for a named group.
 *   preg  : compiled regex_t
 *   name  : group name
 * Returns the 1-based capture group index, or -1 if not found.
 */
int lr_regex_named_group_index(const regex_t *preg, const char *name);

#endif /* LR_REGEX_H */
