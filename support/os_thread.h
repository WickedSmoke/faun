#ifndef OS_THREAD_H
#define OS_THREAD_H
/*
   Thread operating system interface.
*/

#ifdef _WIN32

// Must define _WIN32_WINNT to use InitializeCriticalSectionAndSpinCount
#ifndef _WIN32_WINNT
#define _WIN32_WINNT    0x0403
#endif
#include <windows.h>

typedef HANDLE              pthread_t;
typedef CRITICAL_SECTION    pthread_mutex_t;
typedef CONDITION_VARIABLE  pthread_cond_t;

#define mutexInitF(mh) \
    (InitializeCriticalSectionAndSpinCount(&mh,0x80000400) == 0)
#define mutexFree(mh)       DeleteCriticalSection(&mh)
#define mutexLock(mh)       EnterCriticalSection(&mh)
#define mutexUnlock(mh)     LeaveCriticalSection(&mh)
#define condInit(cond)      InitializeConditionVariable(&cond)
#define condFree(cond)
#define condWaitF(cond,mh)  (! SleepConditionVariableCS(&cond,&mh,INFINITE))
#define condSignal(cond)    WakeConditionVariable(&cond)
#define threadCreateF(th,func,arg)  ((th = CreateThread(NULL,0,func,arg,0,NULL)) == NULL)
#define threadJoin(th)      WaitForSingleObject(th,INFINITE)

#else

#include <pthread.h>

#define mutexInitF(mh)      (pthread_mutex_init(&mh,0) == -1)
#define mutexFree(mh)       pthread_mutex_destroy(&mh)
#define mutexLock(mh)       pthread_mutex_lock(&mh)
#define mutexUnlock(mh)     pthread_mutex_unlock(&mh)
#define condInit(cond)      pthread_cond_init(&cond,0)
#define condFree(cond)      pthread_cond_destroy(&cond)
#define condWaitF(cond,mh)  pthread_cond_wait(&cond,&mh)
#define condSignal(cond)    pthread_cond_signal(&cond)
#define threadCreateF(th,func,arg)  (pthread_create(&th,NULL,func,arg) != 0)
#define threadJoin(th)      pthread_join(th,NULL)

#endif

#endif  /* OS_THREAD_H */
