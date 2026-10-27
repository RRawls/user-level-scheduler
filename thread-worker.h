#ifndef WORKER_T_H
#define WORKER_T_H

#define _GNU_SOURCE

#include <unistd.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>
#include "mutex_types.h"

/* creates a new thread */
int worker_create(worker_t *thread, pthread_attr_t *attr, void *(*function)(void *), void *arg);

/* gives CPU pocession to other user level worker threads voluntarily */
int worker_yield();

/* terminates a thread */
void worker_exit(void *value_ptr);

/* waits for thread termination */
int worker_join(worker_t thread, void **value_ptr);

/* initializes the mutex lock */
int worker_mutex_init(worker_mutex_t *mutex, const pthread_mutexattr_t
												 *mutexattr);

/* aquires the mutex lock */
int worker_mutex_lock(worker_mutex_t *mutex);

/* releases the mutex lock */
int worker_mutex_unlock(worker_mutex_t *mutex);

/* destroys the mutex */
int worker_mutex_destroy(worker_mutex_t *mutex);

#endif
