/* config.h for LR_JS embedded PCRE2 build (hand-written, MinGW/Windows + Unix).
 *
 * Enables the 8-bit library with Unicode support. JIT is intentionally
 * disabled (requires the external SLJIT dependency). See config.h.generic
 * for the full list of tunable macros. */
#ifndef LR_PCRE2_CONFIG_H
#define LR_PCRE2_CONFIG_H

/* ── Standard headers present ─────────────────────────────────────────── */
#define HAVE_ASSERT_H 1
#define HAVE_LIMITS_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_WCHAR_H 1

#if defined(_WIN32)
#define HAVE_WINDOWS_H 1
#endif

#if defined(__unix__) || defined(__APPLE__)
#define HAVE_UNISTD_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#endif

/* ── Library configuration ────────────────────────────────────────────── */
#define SUPPORT_PCRE2_8 1
#define SUPPORT_UNICODE 1
/* Link statically on Windows (no dllimport needed). */
#if defined(_WIN32)
#define PCRE2_STATIC 1
#endif

#define HEAP_LIMIT 20000000
#define LINK_SIZE 2
#define MATCH_LIMIT 10000000
#define MATCH_LIMIT_DEPTH 10000000
#define MAX_NAME_COUNT 10000
#define MAX_NAME_SIZE 128
#define MAX_VARLOOKBEHIND 255
#define NEWLINE_DEFAULT 2
#define PARENS_NEST_LIMIT 250

#define PACKAGE "pcre2"
#define PACKAGE_BUGREPORT ""
#define PACKAGE_NAME "PCRE2"
#define PACKAGE_STRING "PCRE2 10.47"
#define PACKAGE_TARNAME "pcre2"
#define PACKAGE_URL ""
#define PACKAGE_VERSION "10.47"
#define PCRE2_EXPORT
#define VERSION "10.47"

#endif /* LR_PCRE2_CONFIG_H */
