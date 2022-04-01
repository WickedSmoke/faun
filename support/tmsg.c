/*
  tmsg - Thread-safe message queue.

  Copyright (c) 2022 Karl Robillard

  Permission is hereby granted, free of charge, to any person obtaining a
  copy of this software and associated documentation files (the "Software"),
  to deal in the Software without restriction, including without limitation
  the rights to use, copy, modify, merge, publish, distribute, sublicense,
  and/or sell copies of the Software, and to permit persons to whom the
  Software is furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
  THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
  DEALINGS IN THE SOFTWARE.
*/

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tmsg.h"

#if __STDC_VERSION__ < 201112L || defined(__STDC_NO_ATOMICS__)
#define _Atomic
#define atomic_init(vp,x)  *(vp) = x
#else
#include <stdatomic.h>
#endif

//----------------------------------------------------------------------------
// Mutex & Sempahore

#ifdef _WIN32

// Must define _WIN32_WINNT to use InitializeCriticalSectionAndSpinCount
#ifndef _WIN32_WINNT
#define _WIN32_WINNT    0x0403
#endif
#include <windows.h>

typedef HANDLE  Semaphore;
typedef CRITICAL_SECTION    pthread_mutex_t;

#define mutexInitF(mh) \
    (InitializeCriticalSectionAndSpinCount(&mh,2000) == 0)
#define mutexFree(mh)       DeleteCriticalSection(&mh)
#define mutexLock(mh)       EnterCriticalSection(&mh)
#define mutexUnlock(mh)     LeaveCriticalSection(&mh)

#define semaphoreDestroy(sh)    CloseHandle(sh)
static inline int semaphorePost(Semaphore sema) {
    long r;
    return ReleaseSemaphore(sema, 1, &r) ? 0 : GetLastError();
}

#else

#include <pthread.h>
#include <time.h>

#define mutexInitF(mh)      (pthread_mutex_init(&mh,0) == -1)
#define mutexFree(mh)       pthread_mutex_destroy(&mh)
#define mutexLock(mh)       pthread_mutex_lock(&mh)
#define mutexUnlock(mh)     pthread_mutex_unlock(&mh)

#ifdef __APPLE__
#include <dispatch/dispatch.h>
#include <dispatch/time.h>
typedef dispatch_semaphore_t Semaphore;
#define semaphoreDestroy(sema)  dispatch_release(sema)
#define semaphorePost(sema)     dispatch_semaphore_signal(sema)
#else
#include <errno.h>
#include <semaphore.h>
typedef sem_t Semaphore;
#define semaphoreDestroy(sema)  sem_destroy(&sema)
#define semaphorePost(sema)     sem_post(&sema)
#endif

#endif

/*
 * Create an unnamed semaphore.
 * \return zero on success or error code.
 */
static int semaphoreCreate(Semaphore* sema, long value)
{
#ifdef _WIN32
    HANDLE handle = CreateSemaphoreA(NULL, value, 0x7FFFFFFF, NULL);
    if(! handle)
        return GetLastError();
    *sema = handle;
    return 0;
#elif defined(__APPLE__)
    *sema = dispatch_semaphore_create(value);
    return *sema ? 0 : -1;
#else
    return sem_init(sema, 0, value) ? errno : 0;
#endif
}

/*
 * Return 0 on success or error code.
 */
static inline int semaphoreWait(Semaphore* sema)
{
#ifdef _WIN32
    DWORD r = WaitForSingleObjectEx(*sema, INFINITE, TRUE);
    return (r == WAIT_OBJECT_0) ? 0 : GetLastError();
#elif defined(__APPLE__)
    return dispatch_semaphore_wait(*sema, DISPATCH_TIME_FOREVER);
#else
    int r = sem_wait(sema);
    while(-1 == r && EINTR == errno)    // Handle signal interruption.
        r = sem_wait(sema);
    return r;
#endif
}

/*
 * Return 0 if semaphore was locked, 1 on timeout, or -1 on error.
 */
static inline int semaphoreTimedWait(Semaphore* sema, int timeout)
{
#ifdef _WIN32
    DWORD r = WaitForSingleObjectEx(*sema, timeout, TRUE);
    if (r == WAIT_OBJECT_0)
        return 0;
    if (r == WAIT_TIMEOUT)
        return 1;
    return -1;
#elif defined(__APPLE__)
    intptr_t r = dispatch_semaphore_wait(*sema,
                                dispatch_time(DISPATCH_TIME_NOW, timeout));
    return r ? 1 : 0;
#else
    struct timespec ts;
    int r;

#if defined(CLOCK_REALTIME)
    clock_gettime(CLOCK_REALTIME, &ts);
#else
    // Older POSIX systems.  POSIX.1-2008 marks gettimeofday() as obsolete.
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ts.tv_sec  = tv.tv_sec;
    ts.tv_nsec = tv.tv_usec * 1000;
#endif

    ts.tv_sec  += timeout / 1000;
    ts.tv_nsec += (timeout % 1000) * 1000000;

    // Adjust tv_nsec to less than 1000 million to avoid EINVAL.
    if (ts.tv_nsec >= 1000000000)
    {
        ts.tv_nsec -= 1000000000;
        ts.tv_sec  += 1;
    }

    r = sem_timedwait(sema, &ts);
    while (-1 == r && EINTR == errno)   // Handle signal interruption.
        r = sem_timedwait(sema, &ts);
    if (r == 0)
        return 0;
    if (errno == ETIMEDOUT)
        return 1;
    return -1;
#endif
}

//----------------------------------------------------------------------------

struct MsgPort
{
    uint8_t* buf;       // Message buffer.
    int msize;          // Message byte size.
    int avail;          // Maximum number of messages.
    _Atomic int used;   // Number of messages in queue.
    int tail;

    pthread_mutex_t mutex;
    Semaphore reader;
    Semaphore writer;
};

struct MsgPort* tmsg_create(int msgSize, int capacity)
{
    struct MsgPort* mp;
    assert(msgSize > 0 && capacity > 0);
    mp = (struct MsgPort*) malloc(sizeof(*mp) + msgSize * capacity);
    if (mp)
    {
        if (mutexInitF(mp->mutex))
        {
            free(mp);
            return NULL;
        }
        semaphoreCreate(&mp->reader, 0);
        semaphoreCreate(&mp->writer, capacity);

        mp->buf = (uint8_t*) (mp + 1);
        mp->msize = msgSize;
        mp->avail = capacity;
        atomic_init(&mp->used, 0);
        mp->tail = 0;
    }
    return mp;
}

void tmsg_destroy(struct MsgPort* mp)
{
    if (! mp)
        return;

    semaphoreDestroy(mp->reader);
    semaphoreDestroy(mp->writer);
    mutexFree(mp->mutex);
    free(mp);
}

int tmsg_used(struct MsgPort* mp)
{
    return mp->used;
}

int tmsg_push(struct MsgPort* mp, const void* msg)
{
    semaphoreWait(&mp->writer);

    mutexLock(mp->mutex);
    assert(mp->used < mp->avail);
    memcpy(mp->buf + ((mp->tail + mp->used) % mp->avail) * mp->msize,
           msg, mp->msize);
    mp->used += 1;
    mutexUnlock(mp->mutex);

    semaphorePost(mp->reader);
    return 0;
}

int tmsg_pop(struct MsgPort* mp, void* msg)
{
    semaphoreWait(&mp->reader);

    mutexLock(mp->mutex);
    memcpy(msg, mp->buf + mp->tail * mp->msize, mp->msize);
    mp->used -= 1;
    mp->tail = (mp->tail + 1) % mp->avail;
    assert(mp->used >= 0);
    mutexUnlock(mp->mutex);

    semaphorePost(mp->writer);
    return 0;
}

int tmsg_pushTimeout(struct MsgPort* mp, const void* msg, int msec)
{
    int r = semaphoreTimedWait(&mp->writer, msec);
    if (r)
        return r;

    mutexLock(mp->mutex);
    assert(mp->used < mp->avail);
    memcpy(mp->buf + ((mp->tail + mp->used) % mp->avail) * mp->msize, msg,
           mp->msize);
    mp->used += 1;
    mutexUnlock(mp->mutex);

    semaphorePost(mp->reader);
    return 0;
}

int tmsg_popTimeout(struct MsgPort* mp, void* msg, int msec)
{
    int r = semaphoreTimedWait(&mp->reader, msec);
    if (r)
        return r;

    mutexLock(mp->mutex);
    memcpy(msg, mp->buf + mp->tail * mp->msize, mp->msize);
    mp->used -= 1;
    mp->tail = (mp->tail + 1) % mp->avail;
    assert(mp->used >= 0);
    mutexUnlock(mp->mutex);

    semaphorePost(mp->writer);
    return 0;
}
