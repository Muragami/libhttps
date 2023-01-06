/*
    Simple portable locking across Win32 and pthread, bare minimum.
    muragami, muragami@wishray.com, Jason A. Petrasko 2023
    MIT License: https://opensource.org/licenses/MIT    
*/

#ifndef __XTHREAD_H__
#define __XTHREAD_H__

#ifdef _WIN32
// *****************************************************************************************************
// emulate some of pthread on Win32
#include <stdbool.h>
#include <windows.h>
#include <time.h>

typedef CRITICAL_SECTION pthread_mutex_t;
typedef void pthread_mutexattr_t;
typedef void pthread_condattr_t;
typedef CONDITION_VARIABLE pthread_cond_t;

// only include this defintion if we are not using gcc, because it already has one
#ifndef __GNUC__
    struct timespec {
        long tv_sec;
        long tv_nsec;
    };
#endif

int pthread_mutex_init(pthread_mutex_t *mutex, pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);

int pthread_cond_init(pthread_cond_t *cond, pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *cond);
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime);
int pthread_cond_signal(pthread_cond_t *cond);
int pthread_cond_broadcast(pthread_cond_t *cond);

#else
// *****************************************************************************************************
// or... just use pthread
#include <pthread.h>

#endif

// *****************************************************************************************************
// utilities
unsigned int pcthread_get_num_procs();
void ms_to_timespec(struct timespec *ts, unsigned int ms);

#endif