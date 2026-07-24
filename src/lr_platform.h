/*
 * L/R_JS - Cross-Platform Compatibility Layer
 * Supports: Linux, macOS, Windows 7+, BSD
 *
 * This header provides a uniform API across all platforms,
 * mapping POSIX calls to their Windows equivalents.
 *
 * All headers MUST include lr_platform.h instead of <pthread.h>
 * directly to ensure MSVC compatibility.
 */
#ifndef LR_PLATFORM_H
#define LR_PLATFORM_H

/* ── Standard headers (common to all platforms) ────────────────────────── */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>

/* ── Platform detection ────────────────────────────────────────────────── */

#if defined(_WIN32) || defined(_WIN64)
#define LR_PLATFORM_WINDOWS 1
#if _WIN32_WINNT < 0x0601
#undef  _WIN32_WINNT
#define _WIN32_WINNT 0x0601   /* Windows 7+ */
#endif
#else
#define LR_PLATFORM_WINDOWS 0
#endif

#if defined(__linux__)
#define LR_PLATFORM_LINUX 1
#else
#define LR_PLATFORM_LINUX 0
#endif

#if defined(__APPLE__)
#define LR_PLATFORM_MACOS 1
#else
#define LR_PLATFORM_MACOS 0
#endif

#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#define LR_PLATFORM_BSD 1
#else
#define LR_PLATFORM_BSD 0
#endif

/* MSVC compiler detection */
#if defined(_MSC_VER)
#define LR_COMPILER_MSVC 1
#pragma warning(disable: 4996)  /* deprecated POSIX names */
#pragma warning(disable: 4244)  /* conversion, possible loss of data */
#pragma warning(disable: 4267)  /* conversion from size_t */
#pragma warning(disable: 4100)  /* unreferenced formal parameter */
#pragma warning(disable: 4127)  /* conditional expression is constant */

/* ── MSVC POSIX compatibility macros ─────────────────────────────────── */

/* __attribute__ not supported */
#define __attribute__(x)

/* POSIX strdup → _strdup */
#define strdup _strdup

/* POSIX strtok_r → strtok_s (same signature) */
#define strtok_r(str, delim, saveptr)  strtok_s(str, delim, saveptr)

/* POSIX strcasecmp/strncasecmp → _stricmp/_strnicmp */
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp

/* POSIX snprintf/vsnprintf emulation for MSVC < 2015 (VC14 / _MSC_VER 1900) */
#if _MSC_VER < 1900
#define snprintf  _snprintf
#define vsnprintf _vsnprintf
#endif

/* POSIX localtime_r/gmtime_r wrappers */
static __inline struct tm *localtime_r(const time_t *t, struct tm *result)
{
    if (localtime_s(result, t) != 0) return NULL;
    return result;
}
static __inline struct tm *gmtime_r(const time_t *t, struct tm *result)
{
    if (gmtime_s(result, t) != 0) return NULL;
    return result;
}

#else
#define LR_COMPILER_MSVC 0
#endif

/* ── Cross-platform inline keyword ──────────────────────────────────────── */

#if LR_COMPILER_MSVC
#define LR_INLINE static __inline
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#define LR_INLINE static inline
#else
#define LR_INLINE static __inline__
#endif

/* ── DLL Export / Import (for lr_js.h consumer) ───────────────────────── */
/* NOTE: lr_platform.h is internal; lr_js.h handles public API exports.
   This section is only for internal use. */

#if LR_COMPILER_MSVC && !defined(LR_JS_BUILD_STATIC)
#define LR_INTERNAL_API __declspec(dllexport)
#else
#define LR_INTERNAL_API
#endif

/* ── Windows-specific headers ──────────────────────────────────────────── */

#if LR_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <sys/timeb.h>
#include <intrin.h>

/* Link Windows socket library */
#pragma comment(lib, "ws2_32.lib")

/* ── POSIX emulation for Windows ───────────────────────────────────────── */

/* File operations */
#define lr_open       _open
#define lr_close(fd)  _close(fd)
#define lr_read       _read
#define lr_write      _write
#define lr_lseek      _lseek
#define lr_ftruncate  _chsize
#define lr_fstat      _fstat
#define lr_stat       _stat
#define lr_unlink     _unlink
#define lr_access     _access
#define R_OK          4
#define W_OK          2
#define X_OK          0   /* Windows doesn't have execute permission check */

/* Socket operations */
#define lr_socket_close(s)  closesocket(s)
#define lr_socket_errno()   WSAGetLastError()
#define lr_socket_eagain()  (WSAGetLastError() == WSAEWOULDBLOCK)
#define lr_socket_einprog() (WSAGetLastError() == WSAEWOULDBLOCK)
#define MSG_NOSIGNAL        0
#define MSG_DONTWAIT        0

/* pthread support: MSVC uses compatibility layer, MinGW uses native pthread */
#if LR_COMPILER_MSVC
#include "lr_pthread_win.h"
#else
#include <pthread.h>
#endif

/* Process and thread */
#define lr_getpid()  ((int)GetCurrentProcessId())

/* Insomniac sleep */
#define lr_sleep_ms(ms)  Sleep((DWORD)(ms))

/* Directory separator */
#define LR_PATH_SEP  '\\'
#define LR_PATH_SEP_STR "\\"

#else /* ── Linux / macOS / BSD ──────────────────────────────────────────── */

#include <unistd.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <dlfcn.h>

/* File operations */
#define lr_open       open
#define lr_close(fd)  close(fd)
#define lr_read       read
#define lr_write      write
#define lr_lseek      lseek
#define lr_ftruncate  ftruncate
#define lr_fstat      fstat
#define lr_stat       stat
#define lr_unlink     unlink
#define lr_access     access

/* Socket operations */
#define lr_socket_close(s)  close(s)
#define lr_socket_errno()   errno
#define lr_socket_eagain()  (errno == EAGAIN || errno == EWOULDBLOCK)
#define lr_socket_einprog() (errno == EINPROGRESS)

/* Process and thread */
#define lr_getpid()  ((int)getpid())

/* Insomniac sleep */
#define lr_sleep_ms(ms)  usleep((ms) * 1000)

/* Directory separator */
#define LR_PATH_SEP  '/'
#define LR_PATH_SEP_STR "/"

#endif /* LR_PLATFORM_WINDOWS */

/* ── clock_gettime for MSVC (Windows 7+) ────────────────────────────────── */

#if LR_PLATFORM_WINDOWS

/* clockid_t for MSVC */
typedef int clockid_t;

#define CLOCK_REALTIME       0   /* wall-clock time (epoch) */
#define CLOCK_MONOTONIC      1   /* high-resolution monotonic (since boot) */
#define CLOCK_MONOTONIC_RAW  1

/* struct timespec is provided natively by UCRT <time.h> on MSVC (guarded
   internally by _CRT_NO_TIME_T, NOT _TIMESPEC_DEFINED), so we must not
   redefine it there. Only MinGW needs our own definition. */
#if !LR_COMPILER_MSVC
#ifndef _TIMESPEC_DEFINED
#define _TIMESPEC_DEFINED
struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};
#endif
#endif

/* Implementation of clock_gettime for Windows */
LR_INLINE int clock_gettime(clockid_t clk_id, struct timespec *ts)
{
    if (clk_id == CLOCK_REALTIME) {
        /* Wall-clock time. Must be consistent with pthread_cond_timedwait(),
           which derives "now" from _ftime_s() - otherwise timed waits would
           compute a negative timeout and return immediately (busy loop). */
        struct _timeb tb;
        _ftime_s(&tb);
        ts->tv_sec  = (time_t)tb.time;
        ts->tv_nsec = (long)tb.millitm * 1000000L;
        return 0;
    }

    /* CLOCK_MONOTONIC / CLOCK_MONOTONIC_RAW: high-resolution monotonic clock */
    static LARGE_INTEGER freq = {0};
    static int initialized = 0;
    LARGE_INTEGER now;

    if (!initialized) {
        QueryPerformanceFrequency(&freq);
        initialized = 1;
    }
    QueryPerformanceCounter(&now);

    if (freq.QuadPart > 0) {
        ts->tv_sec  = (time_t)(now.QuadPart / freq.QuadPart);
        ts->tv_nsec = (long)(((now.QuadPart % freq.QuadPart) * 1000000000LL) / freq.QuadPart);
    } else {
        ts->tv_sec = 0;
        ts->tv_nsec = 0;
    }
    return 0;
}

/* Sleep for nanoseconds (approximate) */
LR_INLINE int nanosleep(const struct timespec *req, struct timespec *rem)
{
    DWORD ms = (DWORD)(req->tv_sec * 1000 + req->tv_nsec / 1000000);
    if (ms == 0 && req->tv_nsec > 0) ms = 1;
    Sleep(ms);
    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}

#endif /* LR_PLATFORM_WINDOWS (clock_gettime) */

/* ── Time helpers (cross-platform) ─────────────────────────────────────── */

#if LR_PLATFORM_WINDOWS

/* High-resolution timer using QueryPerformanceCounter */
LR_INLINE int64_t lr_get_time_us(void)
{
    static LARGE_INTEGER freq = {0};
    static int initialized = 0;
    LARGE_INTEGER now;

    if (!initialized) {
        QueryPerformanceFrequency(&freq);
        initialized = 1;
    }
    QueryPerformanceCounter(&now);
    return (int64_t)((now.QuadPart * 1000000ULL) / freq.QuadPart);
}

/* Get current time in seconds (for time()) */
LR_INLINE int64_t lr_time_sec(void)
{
    return (int64_t)time(NULL);
}

#else

/* Linux/macOS: gettimeofday */
#include <sys/time.h>

LR_INLINE int64_t lr_get_time_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
}

LR_INLINE int64_t lr_time_sec(void)
{
    return (int64_t)time(NULL);
}

#endif

/* ── Memory info (cross-platform) ──────────────────────────────────────── */

#if LR_PLATFORM_WINDOWS

LR_INLINE long lr_get_page_size(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (long)si.dwPageSize;
}

LR_INLINE long lr_get_avail_mem_pages(void)
{
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    return (long)(ms.ullAvailPhys / lr_get_page_size());
}

#else

LR_INLINE long lr_get_page_size(void)
{
    long sz = sysconf(_SC_PAGESIZE);
    return sz > 0 ? sz : 4096;
}

LR_INLINE long lr_get_avail_mem_pages(void)
{
    long pages = sysconf(_SC_AVPHYS_PAGES);
    return pages > 0 ? pages : (1024 * 256);  /* fallback: 256MB */
}

#endif

/* ── Shared memory / File mapping (cross-platform) ─────────────────────── */

#if LR_PLATFORM_WINDOWS

/* shm_open → CreateFileMapping, mmap → MapViewOfFile */
#define LR_SHM_INVALID  NULL
#define LR_MMAP_FAILED  NULL

#else

#include <sys/mman.h>

#define LR_SHM_INVALID  ((void *)-1)
#define LR_MMAP_FAILED  MAP_FAILED

#endif

/* ── Socket initialization (Winsock) ───────────────────────────────────── */

#if LR_PLATFORM_WINDOWS

LR_INLINE int lr_socket_init(void)
{
    WSADATA wsa;
    static int wsa_initialized = 0;
    if (wsa_initialized) return 0;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
    wsa_initialized = 1;
    return 0;
}

LR_INLINE void lr_socket_cleanup(void)
{
    WSACleanup();
}

#else

LR_INLINE int lr_socket_init(void)    { return 0; }
LR_INLINE void lr_socket_cleanup(void) {}

#endif

/* ── Dynamic library loading ───────────────────────────────────────────── */

#if LR_PLATFORM_WINDOWS

#define lr_dlopen(path)    (void *)LoadLibraryA(path)
#define lr_dlsym(h, name)  (void *)GetProcAddress((HMODULE)(h), name)
#define lr_dlclose(h)      FreeLibrary((HMODULE)(h))
#define lr_dlerror()       "dynamic library error"

#else

#define lr_dlopen(path)    dlopen(path, RTLD_LAZY)
#define lr_dlsym(h, name)  dlsym(h, name)
#define lr_dlclose(h)      dlclose(h)
#define lr_dlerror()       dlerror()

#endif

/* ── Non-blocking socket setup ─────────────────────────────────────────── */

#if LR_PLATFORM_WINDOWS

LR_INLINE int lr_socket_set_nonblock(int sock)
{
    u_long mode = 1;
    return ioctlsocket(sock, FIONBIO, &mode);
}

#else

LR_INLINE int lr_socket_set_nonblock(int sock)
{
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

#endif

/* ── File existence check ──────────────────────────────────────────────── */

LR_INLINE int lr_file_exists(const char *path)
{
    return lr_access(path, R_OK) == 0;
}

/* ── Random number (cross-platform) ────────────────────────────────────── */

#if LR_PLATFORM_WINDOWS

LR_INLINE int lr_random_bytes(unsigned char *buf, size_t len)
{
    /* Use CryptGenRandom for Win7+ */
    HCRYPTPROV prov;
    if (!CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_FULL,
                              CRYPT_VERIFYCONTEXT | CRYPT_SILENT))
        return -1;
    BOOL ok = CryptGenRandom(prov, (DWORD)len, buf);
    CryptReleaseContext(prov, 0);
    return ok ? 0 : -1;
}

#else

LR_INLINE int lr_random_bytes(unsigned char *buf, size_t len)
{
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    size_t r = fread(buf, 1, len, f);
    fclose(f);
    return (r == len) ? 0 : -1;
}

#endif

/* ── Thread-local storage ──────────────────────────────────────────────── */

#if LR_PLATFORM_WINDOWS

#define LR_THREAD_LOCAL  __declspec(thread)

#else

#define LR_THREAD_LOCAL  __thread

#endif

/* ── Atomic operations ─────────────────────────────────────────────────────
 *
 * Provides CAS (Compare-And-Swap), exchange, fetch_add, load, store
 * for both 32-bit integers and pointers.
 * These are the foundation for lock-free data structures.
 * ──────────────────────────────────────────────────────────────────────── */

#if LR_PLATFORM_WINDOWS

/* ── Windows (MSVC) Atomic Operations ──────────────────────────────────── */

/* 32-bit CAS: atomically compare *ptr to oldval, if equal store newval */
LR_INLINE int32_t lr_atomic_cas_32(volatile int32_t *ptr, int32_t oldval, int32_t newval)
{
    return (int32_t)InterlockedCompareExchange((volatile LONG *)ptr, (LONG)newval, (LONG)oldval);
}

/* Pointer CAS */
LR_INLINE void *lr_atomic_cas_ptr(volatile void **ptr, void *oldval, void *newval)
{
    return (void *)InterlockedCompareExchangePointer((PVOID volatile *)ptr, newval, oldval);
}

/* Atomic exchange (swap) */
LR_INLINE int32_t lr_atomic_xchg_32(volatile int32_t *ptr, int32_t val)
{
    return (int32_t)InterlockedExchange((volatile LONG *)ptr, (LONG)val);
}

/* Pointer atomic exchange */
LR_INLINE void *lr_atomic_xchg_ptr(volatile void **ptr, void *val)
{
    return (void *)InterlockedExchangePointer((PVOID volatile *)ptr, val);
}

/* Atomic fetch-and-add */
LR_INLINE int32_t lr_atomic_fetch_add_32(volatile int32_t *ptr, int32_t val)
{
    return (int32_t)InterlockedExchangeAdd((volatile LONG *)ptr, (LONG)val);
}

/* Atomic store with release semantics */
LR_INLINE void lr_atomic_store_32(volatile int32_t *ptr, int32_t val)
{
    InterlockedExchange((volatile LONG *)ptr, (LONG)val);
}

/* Atomic store for pointer */
LR_INLINE void lr_atomic_store_ptr(volatile void **ptr, void *val)
{
    InterlockedExchangePointer((PVOID volatile *)ptr, val);
}

/* Atomic load with acquire semantics */
LR_INLINE int32_t lr_atomic_load_32(volatile int32_t *ptr)
{
    return (int32_t)InterlockedCompareExchange((volatile LONG *)ptr, 0, 0);
}

/* Atomic load for pointer */
LR_INLINE void *lr_atomic_load_ptr(volatile void **ptr)
{
    return (void *)InterlockedCompareExchangePointer((PVOID volatile *)ptr, NULL, NULL);
}

/* Memory barrier (full fence) */
LR_INLINE void lr_memory_barrier(void)
{
    MemoryBarrier();
}

/* Write barrier (release) */
LR_INLINE void lr_write_barrier(void)
{
    MemoryBarrier();
}

/* Read barrier (acquire) */
LR_INLINE void lr_read_barrier(void)
{
    MemoryBarrier();
}

/* 64-bit atomic operations */
LR_INLINE int64_t lr_atomic_cas_64(volatile int64_t *ptr, int64_t oldval, int64_t newval)
{
    return (int64_t)InterlockedCompareExchange64((volatile LONGLONG *)ptr, (LONGLONG)newval, (LONGLONG)oldval);
}

LR_INLINE int64_t lr_atomic_load_64(volatile int64_t *ptr)
{
    return (int64_t)InterlockedCompareExchange64((volatile LONGLONG *)ptr, 0, 0);
}

#else /* ── GCC/Clang (Linux, macOS, BSD) Atomic Operations ────────────────── */

/* 32-bit CAS */
LR_INLINE int32_t lr_atomic_cas_32(volatile int32_t *ptr, int32_t oldval, int32_t newval)
{
    return __sync_val_compare_and_swap(ptr, oldval, newval);
}

/* Pointer CAS */
LR_INLINE void *lr_atomic_cas_ptr(volatile void **ptr, void *oldval, void *newval)
{
    return (void *)__sync_val_compare_and_swap((void *volatile *)ptr, oldval, newval);
}

/* Atomic exchange (swap) - using CAS loop */
LR_INLINE int32_t lr_atomic_xchg_32(volatile int32_t *ptr, int32_t val)
{
    int32_t old;
    do {
        __sync_synchronize();
        old = *ptr;
    } while (!__sync_bool_compare_and_swap(ptr, old, val));
    return old;
}

/* Pointer atomic exchange */
LR_INLINE void *lr_atomic_xchg_ptr(volatile void **ptr, void *val)
{
    void *old;
    do {
        __sync_synchronize();
        old = (void *)*ptr;
    } while (!__sync_bool_compare_and_swap((void *volatile *)ptr, old, val));
    return (void *)old;
}

/* Atomic fetch-and-add */
LR_INLINE int32_t lr_atomic_fetch_add_32(volatile int32_t *ptr, int32_t val)
{
    return __sync_fetch_and_add(ptr, val);
}

/* Atomic store with release semantics */
LR_INLINE void lr_atomic_store_32(volatile int32_t *ptr, int32_t val)
{
    __sync_synchronize();
    *ptr = val;
    __sync_synchronize();
}

/* Atomic store for pointer */
LR_INLINE void lr_atomic_store_ptr(volatile void **ptr, void *val)
{
    __sync_synchronize();
    *ptr = val;
    __sync_synchronize();
}

/* Atomic load with acquire semantics */
LR_INLINE int32_t lr_atomic_load_32(volatile int32_t *ptr)
{
    return __sync_val_compare_and_swap(ptr, 0, 0);
}

/* Atomic load for pointer */
LR_INLINE void *lr_atomic_load_ptr(volatile void **ptr)
{
    return (void *)__sync_val_compare_and_swap((void *volatile *)ptr, NULL, NULL);
}

/* Memory barrier (full fence) */
LR_INLINE void lr_memory_barrier(void)
{
    __sync_synchronize();
}

/* Write barrier */
LR_INLINE void lr_write_barrier(void)
{
    __sync_synchronize();
}

/* Read barrier */
LR_INLINE void lr_read_barrier(void)
{
    __sync_synchronize();
}

/* 64-bit atomic operations */
LR_INLINE int64_t lr_atomic_cas_64(volatile int64_t *ptr, int64_t oldval, int64_t newval)
{
    return __sync_val_compare_and_swap(ptr, oldval, newval);
}

LR_INLINE int64_t lr_atomic_load_64(volatile int64_t *ptr)
{
    return __sync_val_compare_and_swap(ptr, 0, 0);
}

#endif

/* ── Convenience macros for common CAS patterns ─────────────────────────── */

/* Atomically increment a 32-bit counter, return old value */
#define LR_ATOMIC_INC(ptr)       lr_atomic_fetch_add_32((ptr), 1)

/* Atomically decrement a 32-bit counter, return old value */
#define LR_ATOMIC_DEC(ptr)       lr_atomic_fetch_add_32((ptr), -1)

/* Thread-safe flag test-and-set (returns 1 if was already set, 0 if not) */
#define LR_ATOMIC_TEST_AND_SET(ptr)  (lr_atomic_xchg_32((ptr), 1) != 0)

/* Thread-safe flag clear */
#define LR_ATOMIC_CLEAR(ptr)     lr_atomic_store_32((ptr), 0)

/* ── End of platform layer ─────────────────────────────────────────────── */

#endif /* LR_PLATFORM_H */