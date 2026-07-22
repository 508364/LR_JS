/*
 * L/R_JS - Windows pthread compatibility layer for MSVC
 *
 * Provides a thin wrapper around Windows native threading APIs
 * to emulate the most common pthread APIs used by LR_JS.
 *
 * This is used ONLY when compiling with MSVC (cl.exe).
 * MinGW/GCC on Windows already provides native pthread.
 */
#ifndef LR_PTHREAD_WIN_H
#define LR_PTHREAD_WIN_H

#ifdef _MSC_VER

#include <windows.h>
#include <process.h>
#include <sys/timeb.h>

/* ── Mutex ─────────────────────────────────────────────────────────────── */

typedef struct {
    CRITICAL_SECTION cs;
    int              initialized;
} pthread_mutex_t;

typedef struct {
    int dummy;
} pthread_mutexattr_t;

#define PTHREAD_MUTEX_INITIALIZER { {0}, 0 }

static __inline int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr)
{
    (void)attr;
    InitializeCriticalSection(&m->cs);
    m->initialized = 1;
    return 0;
}

static __inline int pthread_mutex_lock(pthread_mutex_t *m)
{
    if (!m->initialized) pthread_mutex_init(m, NULL);
    EnterCriticalSection(&m->cs);
    return 0;
}

static __inline int pthread_mutex_unlock(pthread_mutex_t *m)
{
    LeaveCriticalSection(&m->cs);
    return 0;
}

static __inline int pthread_mutex_destroy(pthread_mutex_t *m)
{
    if (m->initialized) {
        DeleteCriticalSection(&m->cs);
        m->initialized = 0;
    }
    return 0;
}

/* ── Condition Variable (Vista+) ──────────────────────────────────────── */

typedef struct {
    CONDITION_VARIABLE cv;
    int                initialized;
} pthread_cond_t;

typedef struct {
    int dummy;
} pthread_condattr_t;

static __inline int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr)
{
    (void)attr;
    InitializeConditionVariable(&c->cv);
    c->initialized = 1;
    return 0;
}

static __inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    if (!c->initialized) pthread_cond_init(c, NULL);
    SleepConditionVariableCS(&c->cv, &m->cs, INFINITE);
    return 0;
}

static __inline int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                                            const struct timespec *abstime)
{
    if (!c->initialized) pthread_cond_init(c, NULL);
    /* Convert timespec to millisecond delta */
    struct _timeb tb;
    _ftime_s(&tb);
    int64_t now_ms = (int64_t)tb.time * 1000LL + tb.millitm;
    int64_t target_ms = (int64_t)abstime->tv_sec * 1000LL + abstime->tv_nsec / 1000000LL;
    DWORD ms = (DWORD)(target_ms > now_ms ? target_ms - now_ms : 0);
    if (!SleepConditionVariableCS(&c->cv, &m->cs, ms))
        return 1; /* ETIMEDOUT */
    return 0;
}

static __inline int pthread_cond_signal(pthread_cond_t *c)
{
    WakeConditionVariable(&c->cv);
    return 0;
}

static __inline int pthread_cond_broadcast(pthread_cond_t *c)
{
    WakeAllConditionVariable(&c->cv);
    return 0;
}

static __inline int pthread_cond_destroy(pthread_cond_t *c)
{
    (void)c;
    return 0;
}

/* ── Thread ────────────────────────────────────────────────────────────── */

typedef HANDLE pthread_t;

typedef struct {
    size_t stack_size;
} pthread_attr_t;

/* Thread routine signature */
typedef unsigned int (__stdcall *pthread_start_routine_t)(void *);

static __inline int pthread_attr_init(pthread_attr_t *attr)
{
    attr->stack_size = 0;
    return 0;
}

static __inline int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size)
{
    attr->stack_size = size;
    return 0;
}

static __inline int pthread_attr_destroy(pthread_attr_t *attr)
{
    (void)attr;
    return 0;
}

/* Internal structure for passing arguments to the thread */
typedef struct {
    pthread_start_routine_t func;
    void *arg;
} pthread_start_info_t;

static unsigned int __stdcall pthread_start_wrapper(void *arg)
{
    pthread_start_info_t *info = (pthread_start_info_t *)arg;
    pthread_start_routine_t func = info->func;
    void *user_arg = info->arg;
    free(info);
    return func(user_arg);
}

static __inline int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                    pthread_start_routine_t func, void *arg)
{
    pthread_start_info_t *info = (pthread_start_info_t *)malloc(sizeof(*info));
    if (!info) return -1;
    info->func = func;
    info->arg = arg;

    size_t stack = (attr && attr->stack_size) ? attr->stack_size : 0;
    HANDLE h = (HANDLE)_beginthreadex(NULL, (unsigned int)stack,
                                       pthread_start_wrapper, info, 0, NULL);
    if (!h) {
        free(info);
        return -1;
    }
    *thread = h;
    return 0;
}

static __inline int pthread_join(pthread_t thread, void **retval)
{
    DWORD ret = WaitForSingleObject(thread, INFINITE);
    if (retval) *retval = NULL;
    CloseHandle(thread);
    return (ret == WAIT_OBJECT_0) ? 0 : -1;
}

static __inline int pthread_detach(pthread_t thread)
{
    CloseHandle(thread);
    return 0;
}

static __inline pthread_t pthread_self(void)
{
    return GetCurrentThread();
}

static __inline int pthread_equal(pthread_t a, pthread_t b)
{
    return a == b;
}

/* ── Once (one-time initialization) ────────────────────────────────────── */

typedef INIT_ONCE pthread_once_t;
#define PTHREAD_ONCE_INIT INIT_ONCE_STATIC_INIT

static __inline BOOL CALLBACK pthread_once_callback(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)ctx;
    void (*func)(void) = (void (*)(void))param;
    func();
    return TRUE;
}

static __inline int pthread_once(pthread_once_t *once, void (*func)(void))
{
    InitOnceExecuteOnce(once, pthread_once_callback, (PVOID)func, NULL);
    return 0;
}

/* ── TLS Key ───────────────────────────────────────────────────────────── */

typedef DWORD pthread_key_t;

static __inline int pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
    (void)destructor;
    *key = TlsAlloc();
    return (*key == TLS_OUT_OF_INDEXES) ? -1 : 0;
}

static __inline int pthread_key_delete(pthread_key_t key)
{
    TlsFree(key);
    return 0;
}

static __inline void *pthread_getspecific(pthread_key_t key)
{
    return TlsGetValue(key);
}

static __inline int pthread_setspecific(pthread_key_t key, const void *value)
{
    TlsSetValue(key, (LPVOID)value);
    return 0;
}

/* ── Missing POSIX headers ─────────────────────────────────────────────── */

/* sys/time.h struct timeval for MSVC (guard against winsock2.h) */
#ifndef _TIMEVAL_DEFINED
struct timeval {
    long tv_sec;
    long tv_usec;
};
#define _TIMEVAL_DEFINED
#endif

/* gettimeofday for MSVC */
static __inline int gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    struct _timeb tb;
    _ftime_s(&tb);
    tv->tv_sec = (long)tb.time;
    tv->tv_usec = (long)tb.millitm * 1000;
    return 0;
}

/* usleep for MSVC */
static __inline int usleep(unsigned int usec)
{
    Sleep((usec + 999) / 1000);
    return 0;
}

/* unistd.h stubs */
#define R_OK 4
#define W_OK 2
#define F_OK 0

/* ssize_t */
#ifndef _SSIZE_T_DEFINED
typedef long ssize_t;
#define _SSIZE_T_DEFINED
#endif

#endif /* _MSC_VER */

#endif /* LR_PTHREAD_WIN_H */