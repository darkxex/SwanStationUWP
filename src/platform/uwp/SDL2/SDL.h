/*
 * SwanStation Xbox - the few SDL2 calls the frontend uses outside the platform layer (threads, mutexes, condition
 * variables, semaphores, atomics, the clock), on Win32 so a UWP package needs no SDL. Same names, same semantics;
 * found through the include path as <SDL2/SDL.h>.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationXbox_SDL_SHIM_H
#define SwanStationXbox_SDL_SHIM_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <wchar.h>
#include <errno.h>
#include <stdarg.h>
#include <inttypes.h>
#include <limits.h>
#include <malloc.h>

typedef uint8_t Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef uint64_t Uint64;
typedef int8_t Sint8;
typedef int16_t Sint16;
typedef int32_t Sint32;
typedef int64_t Sint64;

typedef int SDL_bool;
#define SDL_TRUE 1
#define SDL_FALSE 0

/* ----------------------------------------------------------------- atomics */
typedef struct
{
    volatile LONG value;
} SDL_atomic_t;

static __inline int SDL_AtomicSet(SDL_atomic_t *a, int v) { return (int)InterlockedExchange(&a->value, (LONG)v); }
static __inline int SDL_AtomicGet(SDL_atomic_t *a) { return (int)InterlockedCompareExchange(&a->value, 0, 0); }
static __inline int SDL_AtomicAdd(SDL_atomic_t *a, int v) { return (int)InterlockedExchangeAdd(&a->value, (LONG)v); }
static __inline SDL_bool SDL_AtomicCAS(SDL_atomic_t *a, int oldval, int newval)
{
    return InterlockedCompareExchange(&a->value, (LONG)newval, (LONG)oldval) == (LONG)oldval;
}

/* ----------------------------------------------------------------- mutexes */
typedef struct SDL_mutex
{
    CRITICAL_SECTION cs;
} SDL_mutex;

static __inline SDL_mutex *SDL_CreateMutex(void)
{
    SDL_mutex *m = (SDL_mutex *)malloc(sizeof(*m));
    if (m)
        InitializeCriticalSectionEx(&m->cs, 4000, 0);
    return m;
}
static __inline void SDL_DestroyMutex(SDL_mutex *m)
{
    if (m)
    {
        DeleteCriticalSection(&m->cs);
        free(m);
    }
}
static __inline int SDL_LockMutex(SDL_mutex *m)
{
    EnterCriticalSection(&m->cs);
    return 0;
}
static __inline int SDL_TryLockMutex(SDL_mutex *m) { return TryEnterCriticalSection(&m->cs) ? 0 : 1; }
static __inline int SDL_UnlockMutex(SDL_mutex *m)
{
    LeaveCriticalSection(&m->cs);
    return 0;
}

/* --------------------------------------------------------------- conditions */
typedef struct SDL_cond
{
    CONDITION_VARIABLE cv;
} SDL_cond;

static __inline SDL_cond *SDL_CreateCond(void)
{
    SDL_cond *c = (SDL_cond *)malloc(sizeof(*c));
    if (c)
        InitializeConditionVariable(&c->cv);
    return c;
}
static __inline void SDL_DestroyCond(SDL_cond *c) { free(c); }
static __inline int SDL_CondSignal(SDL_cond *c)
{
    WakeConditionVariable(&c->cv);
    return 0;
}
static __inline int SDL_CondBroadcast(SDL_cond *c)
{
    WakeAllConditionVariable(&c->cv);
    return 0;
}
static __inline int SDL_CondWait(SDL_cond *c, SDL_mutex *m)
{
    return SleepConditionVariableCS(&c->cv, &m->cs, INFINITE) ? 0 : -1;
}
#define SDL_MUTEX_TIMEDOUT 1
static __inline int SDL_CondWaitTimeout(SDL_cond *c, SDL_mutex *m, uint32_t ms)
{
    if (SleepConditionVariableCS(&c->cv, &m->cs, ms))
        return 0;
    return GetLastError() == ERROR_TIMEOUT ? SDL_MUTEX_TIMEDOUT : -1;
}

/* --------------------------------------------------------------- semaphores */
typedef struct SDL_sem
{
    HANDLE h;
} SDL_sem;

static __inline SDL_sem *SDL_CreateSemaphore(uint32_t initial)
{
    SDL_sem *s = (SDL_sem *)malloc(sizeof(*s));
    if (!s)
        return NULL;
    s->h = CreateSemaphoreExW(NULL, (LONG)initial, 0x7fffffff, NULL, 0, SEMAPHORE_ALL_ACCESS);
    if (!s->h)
    {
        free(s);
        return NULL;
    }
    return s;
}
static __inline void SDL_DestroySemaphore(SDL_sem *s)
{
    if (s)
    {
        CloseHandle(s->h);
        free(s);
    }
}
static __inline int SDL_SemWait(SDL_sem *s) { return WaitForSingleObjectEx(s->h, INFINITE, FALSE) == WAIT_OBJECT_0 ? 0 : -1; }
static __inline int SDL_SemPost(SDL_sem *s) { return ReleaseSemaphore(s->h, 1, NULL) ? 0 : -1; }

/* ------------------------------------------------------------------ threads */
typedef int (*SDL_ThreadFunction)(void *data);

typedef struct SDL_Thread
{
    HANDLE h;
    SDL_ThreadFunction fn;
    void *data;
    int status;
    volatile LONG state; /* 0 running, 1 detached (it frees the record when it ends), 2 ended; exactly one side frees a detached one */
} SDL_Thread;

static __inline unsigned __stdcall SDL_thread_entry_(void *p)
{
    SDL_Thread *t = (SDL_Thread *)p;
    t->status = t->fn(t->data);
    if (InterlockedCompareExchange(&t->state, 2, 0) == 1) /* detached while it ran */
    {
        CloseHandle(t->h);
        free(t);
    }
    return 0;
}

static __inline SDL_Thread *SDL_CreateThread(SDL_ThreadFunction fn, const char *name, void *data)
{
    (void)name;
    SDL_Thread *t = (SDL_Thread *)calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->fn = fn;
    t->data = data;
    /* 1 MiB: the stack the PS5 build gave its workers is not needed; the frontend's workers are shallow */
    t->h = (HANDLE)_beginthreadex(NULL, 1u << 20, SDL_thread_entry_, t, 0, NULL);
    if (!t->h)
    {
        free(t);
        return NULL;
    }
    return t;
}
static __inline void SDL_WaitThread(SDL_Thread *t, int *status)
{
    if (!t)
        return;
    WaitForSingleObjectEx(t->h, INFINITE, FALSE);
    if (status)
        *status = t->status;
    CloseHandle(t->h);
    free(t);
}
static __inline void SDL_DetachThread(SDL_Thread *t)
{
    if (!t)
        return;
    if (InterlockedCompareExchange(&t->state, 1, 0) == 2) /* it had already ended */
    {
        CloseHandle(t->h);
        free(t);
    }
}

/* -------------------------------------------------------------------- clock */
static __inline uint64_t SDL_GetTicks64(void) { return (uint64_t)GetTickCount64(); }
static __inline uint32_t SDL_GetTicks(void) { return (uint32_t)GetTickCount64(); }
static __inline void SDL_Delay(uint32_t ms) { Sleep(ms); }
static __inline char *SDL_getenv(const char *name) { return getenv(name); }

#endif
