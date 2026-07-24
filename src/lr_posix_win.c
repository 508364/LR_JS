/*
 * lr_posix_win.c - Windows implementations of the POSIX shims declared in
 * lr_posix_win.h. Compiled only under MSVC (see CMakeLists.txt).
 */
#include "lr_posix_win.h"

#include <windows.h>
#include <fcntl.h>      /* _O_CREAT, _O_RDWR, _O_EXCL */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef _S_IREAD
#define _S_IREAD  0x0100
#endif
#ifndef _S_IWRITE
#define _S_IWRITE 0x0080
#endif

/* RtlGetVersion prototype (avoids pulling in winternl.h). */
typedef struct {
    ULONG dwOSVersionInfoSize;
    ULONG dwMajorVersion;
    ULONG dwMinorVersion;
    ULONG dwBuildNumber;
    ULONG dwPlatformId;
    WCHAR szCSDVersion[128];
} lr_RTL_OSVERSIONINFOW;

extern __declspec(dllimport) LONG WINAPI RtlGetVersion(lr_RTL_OSVERSIONINFOW *);

/* ── dirent ────────────────────────────────────────────────────────────── */

DIR *opendir(const char *name)
{
    char pattern[MAX_PATH + 16];
    snprintf(pattern, sizeof(pattern), "%s\\*", name);

    DIR *dir = (DIR *)malloc(sizeof(DIR));
    if (!dir)
        return NULL;

    HANDLE h = FindFirstFileA(pattern, (WIN32_FIND_DATAA *)dir->find_data);
    if (h == INVALID_HANDLE_VALUE) {
        free(dir);
        errno = ENOENT;
        return NULL;
    }
    dir->hFind = h;
    dir->first = 1;
    memset(&dir->ent, 0, sizeof(dir->ent));
    return dir;
}

struct dirent *readdir(DIR *dir)
{
    if (!dir)
        return NULL;

    WIN32_FIND_DATAA *fd = (WIN32_FIND_DATAA *)dir->find_data;

    for (;;) {
        if (!dir->first) {
            if (!FindNextFileA(dir->hFind, fd))
                return NULL;
        } else {
            dir->first = 0;
        }

        const char *fn = fd->cFileName;
        if (strcmp(fn, ".") == 0 || strcmp(fn, "..") == 0)
            continue;

        memset(&dir->ent, 0, sizeof(dir->ent));
        strncpy(dir->ent.d_name, fn, sizeof(dir->ent.d_name) - 1);
        return &dir->ent;
    }
}

int closedir(DIR *dir)
{
    if (!dir)
        return -1;
    if (dir->hFind != INVALID_HANDLE_VALUE)
        FindClose(dir->hFind);
    free(dir);
    return 0;
}

/* ── mkstemp ───────────────────────────────────────────────────────────── */

int mkstemp(char *template)
{
    char *name = _mktemp(template);
    if (!name) {
        errno = EINVAL;
        return -1;
    }
    int fd = _open(name, _O_CREAT | _O_RDWR | _O_EXCL, _S_IREAD | _S_IWRITE);
    if (fd < 0)
        return -1;
    return fd;
}

/* ── strndup ───────────────────────────────────────────────────────────── */

char *lr_strndup(const char *s, size_t n)
{
    size_t len = strlen(s);
    if (len > n)
        len = n;
    char *p = (char *)malloc(len + 1);
    if (!p)
        return NULL;
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

/* ── uname ─────────────────────────────────────────────────────────────── */

int uname(struct utsname *buf)
{
    memset(buf, 0, sizeof(*buf));
    strncpy(buf->sysname, "Windows", sizeof(buf->sysname) - 1);

    lr_RTL_OSVERSIONINFOW vi;
    memset(&vi, 0, sizeof(vi));
    vi.dwOSVersionInfoSize = sizeof(vi);
    RtlGetVersion(&vi);

    snprintf(buf->release, sizeof(buf->release) - 1,
             "%u.%u.%u", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
    snprintf(buf->version, sizeof(buf->version) - 1,
             "Windows %u.%u", vi.dwMajorVersion, vi.dwMinorVersion);

    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    const char *arch = "unknown";
    switch (si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: arch = "x86_64"; break;
        case PROCESSOR_ARCHITECTURE_INTEL:  arch = "x86";    break;
        case PROCESSOR_ARCHITECTURE_ARM64:  arch = "aarch64"; break;
        case PROCESSOR_ARCHITECTURE_ARM:    arch = "arm";    break;
        default:                             arch = "unknown"; break;
    }
    strncpy(buf->machine, arch, sizeof(buf->machine) - 1);

    char host[256];
    DWORD hlen = sizeof(host);
    if (GetComputerNameA(host, &hlen))
        strncpy(buf->nodename, host, sizeof(buf->nodename) - 1);

    return 0;
}

/* ── sysinfo ───────────────────────────────────────────────────────────── */

int sysinfo(struct sysinfo *info)
{
    memset(info, 0, sizeof(*info));
    info->uptime = (long)(GetTickCount64() / 1000ULL);

    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        info->totalram = (unsigned long)(ms.ullTotalPhys / 1024ULL);
        info->freeram  = (unsigned long)(ms.ullAvailPhys / 1024ULL);
    }
    return 0;
}

/* ── sysconf ───────────────────────────────────────────────────────────── */

long sysconf(int name)
{
    if (name == _SC_NPROCESSORS_ONLN) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        return (long)si.dwNumberOfProcessors;
    }
    return -1;
}

/* ── gethostname ───────────────────────────────────────────────────────── */

int lr_gethostname(char *name, size_t len)
{
    char buf[256];
    DWORD n = sizeof(buf);
    if (!GetComputerNameA(buf, &n)) {
        errno = EFAULT;
        return -1;
    }
    if (n >= len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(name, buf, n);
    name[n] = '\0';
    return 0;
}

/* ── temp-file template helper ─────────────────────────────────────────── */

void lr_mkstemp_template(char *buf, size_t buflen, const char *name)
{
    char tmp[MAX_PATH];
    DWORD n = GetTempPathA(sizeof(tmp), tmp);
    if (n == 0 || n >= sizeof(tmp)) {
        tmp[0] = '.';
        tmp[1] = '\0';
    }
    snprintf(buf, buflen, "%s%s_XXXXXX", tmp, name);
}
