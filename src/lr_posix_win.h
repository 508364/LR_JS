/*
 * lr_posix_win.h - Minimal POSIX compatibility shim for MSVC / Windows.
 *
 * This header provides the small subset of POSIX APIs that the Unix-only
 * runtime modules (lr_fs.c, lr_terminal.c, lr_sysinfo.c) depend on, so the
 * whole project (including the lr_js CLI executable) can be built with MSVC
 * without relying on third-party POSIX layers.
 *
 * It is only meaningful on Windows; the modules include it from inside
 * `#ifdef _WIN32` blocks (AFTER lr_runtime.h, which pulls in winsock2.h),
 * so Unix builds keep using their native headers and the SDK's own `stat`
 * declaration is not clobbered by our macros.
 */
#ifndef LR_POSIX_WIN_H
#define LR_POSIX_WIN_H

#ifdef _WIN32

#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <io.h>        /* _open, _mktemp, _close, _strdup */
#include <direct.h>   /* _mkdir, _rmdir, _unlink */
#include <process.h>  /* _popen, _pclose */

/*
 * stat / struct stat: MSVC already provides both via <sys/stat.h> (mapped to
 * _stat / struct _stat). We simply reuse them and supply the S_IS* macros that
 * the SDK may omit. The mode bits (_S_IFDIR / _S_IFREG) match the POSIX values.
 */
#include <sys/stat.h>

#ifndef S_IFMT
#define S_IFMT  _S_IFMT
#endif
#ifndef S_IFDIR
#define S_IFDIR _S_IFDIR
#endif
#ifndef S_IFREG
#define S_IFREG _S_IFREG
#endif
#ifndef S_ISDIR
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#endif

/* ── filesystem operations ─────────────────────────────────────────────── */

#define mkdir(path, mode)  _mkdir(path)
#define rmdir(path)        _rmdir(path)
#define unlink(path)       _unlink(path)
/* rename() and close() are provided natively by MSVC (<stdio.h>/<io.h>).
 * close() is aliased to _close for clarity/consistency with POSIX; if the SDK
 * already defines it, the redefinition is identical and harmless. */
#define close(fd)          _close(fd)

/* ── popen / pclose ────────────────────────────────────────────────────── */

#define popen(cmd, mode)   _popen(cmd, mode)
#define pclose(stream)     _pclose(stream)

/* ── dirent emulation (via FindFirstFile/FindNextFile) ─────────────────── */

#define LR_DIR_NAME_MAX 512

struct dirent {
    char d_name[LR_DIR_NAME_MAX];
};

typedef struct lr_DIR {
    void       *hFind;                 /* HANDLE */
    int         first;                 /* first read pending */
    struct dirent ent;                 /* last returned entry */
    unsigned char find_data[592];      /* opaque WIN32_FIND_DATAA buffer */
} DIR;

DIR           *opendir(const char *name);
struct dirent *readdir(DIR *dir);
int            closedir(DIR *dir);

/* ── mkstemp ───────────────────────────────────────────────────────────── */

int mkstemp(char *template);

/* ── strndup / strdup ──────────────────────────────────────────────────── */

char *lr_strndup(const char *s, size_t n);
#ifndef strndup
#define strndup(s, n) lr_strndup(s, n)
#endif

#ifndef strdup
#define strdup(p) _strdup(p)
#endif

/* ── utsname / uname ───────────────────────────────────────────────────── */

#define _UTSNAME_LENGTH 256
struct utsname {
    char sysname[_UTSNAME_LENGTH];
    char nodename[_UTSNAME_LENGTH];
    char release[_UTSNAME_LENGTH];
    char version[_UTSNAME_LENGTH];
    char machine[_UTSNAME_LENGTH];
};
int uname(struct utsname *buf);

/* ── sysinfo ───────────────────────────────────────────────────────────── */

struct sysinfo {
    long          uptime;     /* seconds */
    unsigned long totalram;   /* kB */
    unsigned long freeram;    /* kB */
};
int sysinfo(struct sysinfo *info);

/* ── sysconf ───────────────────────────────────────────────────────────── */

#define _SC_NPROCESSORS_ONLN 1
long sysconf(int name);

/* ── gethostname ──────────────────────────────────────────────────────────
 * winsock2.h already declares gethostname(); we implement our own via
 * GetComputerNameA and redirect the name so the modules call ours. This macro
 * is defined here (included AFTER lr_runtime.h / winsock2.h) so it does not
 * rewrite winsock's own declaration. */
int lr_gethostname(char *name, size_t len);
#define gethostname(n, l) lr_gethostname(n, l)

/* ── temp-file template helper (terminal module) ───────────────────────── */

void lr_mkstemp_template(char *buf, size_t buflen, const char *name);

#endif /* _WIN32 */
#endif /* LR_POSIX_WIN_H */
