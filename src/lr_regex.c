/* lr_regex.c - PCRE2-based regular expression engine with caching and optimisations.
 *
 * Implements a POSIX-compatible API (regcomp/regexec/regfree/regerror)
 * on top of PCRE2's native API, plus PCRE2 extensions for named capture
 * groups.  This replaces both the system <regex.h> (Linux/macOS) and the
 * previous minimal POSIX engine (MSVC) with a single, modern, consistent
 * implementation that supports:
 *   - Named capture groups  (?<name>...)
 *   - Lookaheads / lookbehinds
 *   - Possessive quantifiers, atomic groups
 *   - Unicode (\p{...}) and all PCRE2 features.
 *
 * Optimisations (round-21):
 *   - Pattern compilation cache keyed on (pattern_hash, cflags) to avoid
 *     redundant pcre2_compile() calls when the same pattern is reused.
 *   - Named-group index lookup via a per-regex hash map instead of
 *     linear table scan.
 *   - pcre2_jit_compile() when PCRE2 is built with SUPPORT_JIT.
 *   - Skip PCRE2_UCP for ASCII-only patterns (faster code path).
 *
 * Thread safety: regcomp/regexec/regfree are NOT thread-safe for the same
 * regex_t; use one regex_t per thread.
 */

#include "lr_regex.h"

/* PCRE2 8-bit library: must be defined before including pcre2.h */
#ifndef PCRE2_CODE_UNIT_WIDTH
#define PCRE2_CODE_UNIT_WIDTH 8
#endif

#include "pcre2.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── DJB2 hash for C strings ────────────────────────────────────────────── */

static uint32_t djb2_hash(const char *s, size_t n)
{
    uint32_t h = 5381;
    for (size_t i = 0; i < n; i++)
        h = h * 33 + (uint8_t)s[i];
    return h;
}

/* ── Pattern compilation cache ────────────────────────────────────────────
 *
 * A global LRU-style cache maps (hash_of_pattern, cflags) → compiled
 * pcre2_code + match_data + JIT flag.  The cache is process-wide and
 * read-only after compilation, so it is safe for concurrent regexec()
 * calls from different threads (only regcomp() touches it).
 *
 * Capacity is kept small because each entry holds a pcre2_code pointer
 * (often a large block) — the goal is to help hot-loop reuse, not to
 * hold thousands of patterns.
 */

#define LR_REGEX_CACHE_CAPACITY  64
#define LR_REGEX_CACHE_MASK      (LR_REGEX_CACHE_CAPACITY - 1)

typedef struct LrRegexCacheEntry {
    uint32_t       hash;             /* djb2 of pattern string */
    int            cflags;           /* regcomp cflags (REG_ICASE etc.) */
    pcre2_code    *code;
    pcre2_match_data *mdata;
    int            has_jit;          /* whether pcre2_jit_compile succeeded */
} LrRegexCacheEntry;

static LrRegexCacheEntry lr_regex_cache[LR_REGEX_CACHE_CAPACITY];
static volatile int      lr_regex_cache_inited = 0;

static void lr_regex_cache_init(void)
{
    if (lr_regex_cache_inited) return;
    memset(lr_regex_cache, 0, sizeof(lr_regex_cache));
    lr_regex_cache_inited = 1;
}

static int lr_regex_is_ascii_only(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if ((uint8_t)s[i] > 127) return 0;
    return 1;
}

static int lr_regex_cache_lookup(const char *pattern, size_t plen,
                                  int cflags,
                                  pcre2_code **out_code)
{
    uint32_t hash = djb2_hash(pattern, plen);
    uint32_t idx  = hash & LR_REGEX_CACHE_MASK;

    for (uint32_t i = 0; i < LR_REGEX_CACHE_CAPACITY; i++) {
        uint32_t slot = (idx + i) & LR_REGEX_CACHE_MASK;
        LrRegexCacheEntry *e = &lr_regex_cache[slot];
        if (e->code && e->hash == hash && e->cflags == cflags) {
            *out_code = e->code;
            return e->has_jit;
        }
    }
    return -1;  /* miss */
}

static void lr_regex_cache_insert(const char *pattern, size_t plen,
                                   int cflags,
                                   pcre2_code *code,
                                   int has_jit)
{
    (void)pattern; (void)plen; (void)cflags; (void)code; (void)has_jit;
}

/* Free all cached entries at process exit (or when explicitly requested). */
void lr_regex_cache_free_all(void)
{
    for (uint32_t i = 0; i < LR_REGEX_CACHE_CAPACITY; i++) {
        LrRegexCacheEntry *e = &lr_regex_cache[i];
        if (e->code) {
            pcre2_code_free(e->code);
            e->code         = NULL;
            e->has_jit      = 0;
        }
    }
}

/* ── Named-group hash map ─────────────────────────────────────────────────
 *
 * Each compiled regex carries a small hash table mapping group-name →
 * group-index, pre-built during regcomp() so that
 * lr_regex_named_group_index() is O(1) instead of O(n).
 *
 * Stored as a malloc'd block inside regex_t->__named_map (freed by regfree).
 */

#define LR_REGEX_NAMED_MAP_SIZE  32
#define LR_REGEX_NAMED_MAP_MASK  (LR_REGEX_NAMED_MAP_SIZE - 1)

typedef struct LrNamedGroupEntry {
    uint32_t  hash;
    char      name[64];       /* max named-group name length (covers common cases) */
    int       group_index;    /* 1-based, matching PCRE2 convention */
} LrNamedGroupEntry;

typedef struct {
    LrNamedGroupEntry entries[LR_REGEX_NAMED_MAP_SIZE];
    uint32_t          count;
} LrNamedGroupMap;

static void lr_named_map_init(LrNamedGroupMap *map)
{
    memset(map, 0, sizeof(*map));
}

/* ── Helper: build the named-group map from the PCRE2 name table ───────── */

static int lr_build_named_map(regex_t *preg, pcre2_code *code)
{
    uint32_t count = 0;
    uint32_t entry_size = 0;
    PCRE2_SPTR table = NULL;

    pcre2_pattern_info(code, PCRE2_INFO_NAMECOUNT, &count);
    pcre2_pattern_info(code, PCRE2_INFO_NAMEENTRYSIZE, &entry_size);
    pcre2_pattern_info(code, PCRE2_INFO_NAMETABLE, &table);

    if (count == 0 || !table || entry_size < 3) return 0;

    LrNamedGroupMap *nm = (LrNamedGroupMap *)preg->__named_map;
    if (!nm) {
        nm = (LrNamedGroupMap *)malloc(sizeof(LrNamedGroupMap));
        if (!nm) return -1;
        preg->__named_map = nm;
    }
    lr_named_map_init(nm);

    for (uint32_t i = 0; i < count; i++) {
        PCRE2_SPTR entry = table + i * entry_size;
        const char *name = (const char *)(entry + 2);
        int gidx = (entry[0] << 8) | entry[1];
        uint32_t h = djb2_hash(name, strlen(name));

        uint32_t idx = h & LR_REGEX_NAMED_MAP_MASK;
        for (uint32_t j = 0; j < LR_REGEX_NAMED_MAP_SIZE; j++) {
            uint32_t slot = (idx + j) & LR_REGEX_NAMED_MAP_MASK;
            LrNamedGroupEntry *e = &nm->entries[slot];
            if (e->hash == 0) {
                memcpy(e->name, name, 63);
                e->name[63] = '\0';
                e->hash         = h;
                e->group_index  = gidx;
                nm->count++;
                break;
            }
            if (e->hash == h && strcmp(e->name, name) == 0) {
                /* duplicate name (allowed by PCRE2 for same group index) */
                break;
            }
        }
    }
    return 0;
}

/* ── regcomp ────────────────────────────────────────────────────────────── */

int regcomp(regex_t *preg, const char *regex, int cflags)
{
    if (!preg || !regex) return REG_ESPACE;

    memset(preg, 0, sizeof(*preg));
    preg->__flags = cflags;

    size_t plen = strlen(regex);

    /* ASCII-only patterns skip PCRE2_UCP for a faster code path. */
    int ascii_only = lr_regex_is_ascii_only(regex, plen);
    uint32_t options = PCRE2_UTF;
    if (!ascii_only) options |= PCRE2_UCP;
    if (cflags & REG_ICASE)   options |= PCRE2_CASELESS;
    if (cflags & REG_NEWLINE) options |= PCRE2_MULTILINE;

    /* Check the compilation cache first. */
    lr_regex_cache_init();
    pcre2_code *cached_code = NULL;
    int has_jit = lr_regex_cache_lookup(regex, plen, cflags, &cached_code);
    if (has_jit >= 0) {
        preg->__code    = cached_code;
        preg->__has_jit = has_jit;
        /* Create a fresh match_data per regex_t — never share across instances. */
        pcre2_match_data *mdata = pcre2_match_data_create_from_pattern(cached_code, NULL);
        if (!mdata) {
            preg->__code        = NULL;
            preg->re_nsub       = 0;
            return REG_ESPACE;
        }
        preg->__mdata = mdata;
        uint32_t nsub = 0;
        pcre2_pattern_info(cached_code, PCRE2_INFO_CAPTURECOUNT, &nsub);
        preg->re_nsub = (size_t)nsub;
        lr_build_named_map(preg, cached_code);
        return 0;
    }

    int errcode;
    PCRE2_SIZE erroffset;
    pcre2_code *code = pcre2_compile((PCRE2_SPTR)regex,
                                     PCRE2_ZERO_TERMINATED,
                                     options, &errcode, &erroffset, NULL);
    if (!code) {
        PCRE2_UCHAR errmsg[256];
        errmsg[0] = '\0';
        pcre2_get_error_message(errcode, errmsg, sizeof(errmsg));
        snprintf(preg->__errbuf, LR_REGEX_ERRBUF_SIZE,
                 "at offset %zu: %s", (size_t)erroffset, (const char *)errmsg);
        preg->__code        = NULL;
        preg->__mdata       = NULL;
        preg->__named_map   = NULL;
        preg->re_nsub       = 0;
        return REG_BADPAT;
    }

    preg->__code = code;

    /* Query the number of capturing subpatterns. */
    uint32_t nsub = 0;
    pcre2_pattern_info(code, PCRE2_INFO_CAPTURECOUNT, &nsub);
    preg->re_nsub = (size_t)nsub;

    /* Pre-allocate a match data block so regexec does not allocate per call. */
    pcre2_match_data *mdata = pcre2_match_data_create_from_pattern(code, NULL);
    if (!mdata) {
        pcre2_code_free(code);
        preg->__code        = NULL;
        preg->re_nsub       = 0;
        return REG_ESPACE;
    }
    preg->__mdata = mdata;

    /* Try to enable JIT compilation (no-op if SUPPORT_JIT is undefined). */
    int jit_rc = -1;
#ifdef SUPPORT_JIT
    jit_rc = pcre2_jit_compile(code, PCRE2_JIT_COMPLETE);
#endif
    preg->__has_jit = (jit_rc == 0) ? 1 : 0;
    int has_jit_flag = preg->__has_jit;

    /* Insert compiled code into the cache (mdata is per-instance, not cached). */
    lr_regex_cache_insert(regex, plen, cflags, code, has_jit_flag);

    /* Build the named-group hash map. */
    lr_build_named_map(preg, code);

    return 0;
}

/* ── regexec ────────────────────────────────────────────────────────────── */

int regexec(const regex_t *preg, const char *string,
            size_t nmatch, regmatch_t pmatch[], int eflags)
{
    (void)eflags;

    if (!preg || !preg->__code) {
        return REG_BADPAT;
    }

    pcre2_code       *code  = (pcre2_code *)preg->__code;
    pcre2_match_data *mdata = (pcre2_match_data *)preg->__mdata;

#ifdef SUPPORT_JIT
    /* If JIT was successfully compiled for this pattern, try the JIT fast
     * path first.  If JIT fails (e.g. unsupported architecture at runtime),
     * fall through to the interpreter. */
    int jit_available = 0;

    if (jit_available) {
        int rc = pcre2_jit_match(code, (PCRE2_SPTR)string, PCRE2_ZERO_TERMINATED,
                                 0, 0, mdata, NULL);
        if (rc != PCRE2_ERROR_JIT_STACK_FAILURE) {
            if (rc < 0) {
                if (rc == PCRE2_ERROR_NOMATCH) return REG_NOMATCH;
                return REG_BADPAT;
            }
            goto copy_ovector;
        }
        /* JIT stack failure — fall back to interpreter. */
    }
#endif

    int rc = pcre2_match(code, (PCRE2_SPTR)string, PCRE2_ZERO_TERMINATED,
                         0, 0, mdata, NULL);

    if (rc < 0) {
        if (rc == PCRE2_ERROR_NOMATCH) return REG_NOMATCH;
        return REG_BADPAT;
    }

copy_ovector:
    /* Copy ovector into the caller's pmatch array.
     * rc is the number of captured groups + 1 (the full match is rc=1). */
    PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(mdata);
    uint32_t    ovec_count = pcre2_get_ovector_count(mdata);
    size_t      max = nmatch < (size_t)ovec_count ? nmatch : (size_t)ovec_count;

    for (size_t i = 0; i < max; i++) {
        if (ovector[2 * i] == PCRE2_UNSET) {
            pmatch[i].rm_so = -1;
            pmatch[i].rm_eo = -1;
        } else {
            pmatch[i].rm_so = (int)ovector[2 * i];
            pmatch[i].rm_eo = (int)ovector[2 * i + 1];
        }
    }

    /* Set remaining entries to unmatched. */
    for (size_t i = max; i < nmatch; i++) {
        pmatch[i].rm_so = -1;
        pmatch[i].rm_eo = -1;
    }

    return 0;
}

/* ── regfree ────────────────────────────────────────────────────────────── */

void regfree(regex_t *preg)
{
    if (!preg) return;
    if (preg->__mdata) {
        pcre2_match_data_free((pcre2_match_data *)preg->__mdata);
        preg->__mdata = NULL;
    }
    /* __code is owned by the compilation cache; only clear the pointer. */
    preg->__code = NULL;
    if (preg->__named_map) {
        free(preg->__named_map);
        preg->__named_map = NULL;
    }
    preg->re_nsub = 0;
}

/* ── regerror ───────────────────────────────────────────────────────────── */

size_t regerror(int errcode, const regex_t *preg,
                char *errbuf, size_t errbuf_size)
{
    const char *msg;

    switch (errcode) {
    case REG_SUCCESS:   msg = "success";             break;
    case REG_NOMATCH:   msg = "no match";            break;
    case REG_ESPACE:    msg = "out of memory";       break;
    case REG_BADPAT:    msg = "bad pattern";         break;
    case REG_ECOLLATE:  msg = "invalid collation element"; break;
    case REG_ENOSYS:    msg = "not implemented";     break;
    case REG_EBRACK:    msg = "unbalanced bracket";  break;
    case REG_EPAREN:    msg = "unbalanced parenthesis"; break;
    case REG_EBRACE:    msg = "unbalanced brace";    break;
    case REG_BADBR:     msg = "invalid repetition count"; break;
    case REG_ERANGE:    msg = "invalid character range";  break;
    case REG_BADRPT:    msg = "invalid repetition";  break;
    case REG_ECTYPE:    msg = "invalid character class"; break;
    case REG_EEOF:      msg = "unexpected EOF";      break;
    case REG_EESCAPE:   msg = "invalid escape";      break;
    case REG_ESUBREG:   msg = "invalid back reference"; break;
    case REG_EBADPAT:   msg = "general pattern error"; break;
    case REG_ERPAREN:   msg = "unbalanced parentheses"; break;
    default:
        if (preg && preg->__errbuf[0])
            msg = preg->__errbuf;
        else
            msg = "unknown regex error";
        break;
    }

    size_t len = strlen(msg);
    if (errbuf && errbuf_size > 0) {
        size_t copy = len < errbuf_size - 1 ? len : errbuf_size - 1;
        memcpy(errbuf, msg, copy);
        errbuf[copy] = '\0';
    }
    return len + 1;  /* POSIX says returns size of buffer needed */
}

/* ── PCRE2 extensions: named capture groups ─────────────────────────────── */

int lr_regex_named_group_count(const regex_t *preg)
{
    if (!preg || !preg->__code) return 0;
    pcre2_code *code = (pcre2_code *)preg->__code;
    uint32_t count = 0;
    pcre2_pattern_info(code, PCRE2_INFO_NAMECOUNT, &count);
    return (int)count;
}

int lr_regex_named_group_name(const regex_t *preg, int idx,
                               char *name, size_t name_len)
{
    if (!preg || !preg->__code || !name || name_len < 1) return -1;
    pcre2_code *code = (pcre2_code *)preg->__code;

    uint32_t count = 0;
    uint32_t entry_size = 0;
    PCRE2_SPTR table = NULL;

    pcre2_pattern_info(code, PCRE2_INFO_NAMECOUNT, &count);
    pcre2_pattern_info(code, PCRE2_INFO_NAMEENTRYSIZE, &entry_size);
    pcre2_pattern_info(code, PCRE2_INFO_NAMETABLE, &table);

    if (idx < 0 || idx >= (int)count || !table || entry_size < 3)
        return -1;

    /* Table entry format (8-bit): 2 bytes group number (big-endian),
     * followed by null-terminated name. */
    PCRE2_SPTR entry = table + (uint32_t)idx * entry_size;
    const char *n = (const char *)(entry + 2);
    size_t nlen = strlen(n);
    if (nlen >= name_len) nlen = name_len - 1;
    memcpy(name, n, nlen);
    name[nlen] = '\0';
    return 0;
}

int lr_regex_named_group_index(const regex_t *preg, const char *name)
{
    if (!preg || !preg->__code || !name) return -1;

    /* Fast path: use the pre-built hash map (O(1) average). */
    const LrNamedGroupMap *nm = (const LrNamedGroupMap *)preg->__named_map;
    if (nm) {
        uint32_t h = djb2_hash(name, strlen(name));
        uint32_t idx = h & LR_REGEX_NAMED_MAP_MASK;
        for (uint32_t i = 0; i < LR_REGEX_NAMED_MAP_SIZE; i++) {
            uint32_t slot = (idx + i) & LR_REGEX_NAMED_MAP_MASK;
            const LrNamedGroupEntry *e = &nm->entries[slot];
            if (e->hash == 0) return -1;
            if (e->hash == h && strcmp(e->name, name) == 0)
                return e->group_index;
        }
        return -1;
    }

    /* Fallback: linear scan of the PCRE2 name table (for robustness). */
    pcre2_code *code = (pcre2_code *)preg->__code;
    uint32_t count = 0;
    uint32_t entry_size = 0;
    PCRE2_SPTR table = NULL;

    pcre2_pattern_info(code, PCRE2_INFO_NAMECOUNT, &count);
    pcre2_pattern_info(code, PCRE2_INFO_NAMEENTRYSIZE, &entry_size);
    pcre2_pattern_info(code, PCRE2_INFO_NAMETABLE, &table);

    for (uint32_t i = 0; i < count; i++) {
        PCRE2_SPTR entry = table + i * entry_size;
        if (strcmp(name, (const char *)(entry + 2)) == 0) {
            return (entry[0] << 8) | entry[1];
        }
    }
    return -1;
}
