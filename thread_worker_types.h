#ifndef TW_TYPES_H
#define TW_TYPES_H

#include <ucontext.h>

typedef unsigned int worker_t;

enum status{ BLOCKED, READY, SCHEDULED, RUNNING, FINISHED };

/* thread control block struct definition */
typedef struct TCB
{
	worker_t threadID;
	enum status state;
	ucontext_t context;
	void* stack;
	int priority;
    void *retVal;
    worker_t joinThreadID;
    void *(*fn)(void *);   //thread entry function
    void *arg;             //thread entry argument

} tcb;

#endif
