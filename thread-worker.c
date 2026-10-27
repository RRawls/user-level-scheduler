#include "thread-worker.h"
#include "thread_worker_types.h"
#include <sys/time.h>
#include <signal.h>
#include <string.h>

#define STACK_SIZE (16 * 1024)
#define QUANTUM (10 * 1000) //for tracking how time between swaps

int init_scheduler_done = 0; 
int nextID = 1; 

ucontext_t scheduler;

struct node{
    tcb *control;
    struct node* next;
};

/* 
* head is always the running thread or the thread that just ran while the scheduler 
* decides the next thread to run; all threads that have not been joined (including BLOCKED
* and FINISHED ones) are stored in the list
*/
struct node* head = NULL;
struct node* mainNode = NULL;

struct sigaction sa;
struct itimerval timer;
sigset_t profSet;

/* 
* the library is not re-entrant, so SIGPROF is blocked while inside it; since its calls never nest,
* a plain block/unblock pair is enough. 
*/
static void libEnter(){ sigprocmask(SIG_BLOCK, &profSet, NULL); }
static void libLeave(){ sigprocmask(SIG_UNBLOCK, &profSet, NULL); }

/* adds threads to the linked list */
void enqueue(struct node* add){
    add->next = NULL;
    if(head == NULL){
        head = add;
    }
    else{
        struct node* track = head;
        while(track->next != NULL){
           track = track->next; 
        }
        track->next = add;
        track->next->next = NULL;
    }
}

/* removes threads from the linked list */
struct node* dequeue(worker_t threadID){
    if(head == NULL){
        return NULL;
    }
    else if(head->control->threadID == threadID){
        struct node* temp = head;
        head = head->next;
        return temp;
    }
    else if(head->next != NULL){
        struct node* prev = head;
        struct node* curr = head->next;
        while(curr != NULL){
            if(curr->control->threadID == threadID){
                break;
            }
            prev = curr;
            curr = curr->next; 
        }
        if(curr != NULL){
            prev->next = curr->next;
            return curr;
        }
    }
    return NULL;
}

/* run when a thread hits its time limit to swap context to the next one */
void timeUp(int signal){
    if(head != NULL && head->control->state == SCHEDULED){
        swapcontext(&(head->control->context), &scheduler);
    }
}

//for the below initialization of the context
static void schedule(); 
static void sched_mlfq();

/* 
* the starting place of every worker thread; "returning" from the thread function behaves
* like calling worker_exit(return value)
*/
static void threadEntry(){
    libLeave(); //new contexts are created with SIGPROF blocked
    void* ret = head->control->fn(head->control->arg);
    worker_exit(ret);
}

/* allocates required memory for the scheduler and sets up the clock */
void initialize(){
    sigemptyset(&profSet);
    sigaddset(&profSet, SIGPROF);

    void* stack = malloc(STACK_SIZE);
    getcontext(&scheduler);
    scheduler.uc_link=NULL;
    scheduler.uc_stack.ss_sp=stack;
    scheduler.uc_stack.ss_size=STACK_SIZE;
    scheduler.uc_stack.ss_flags=0;
    makecontext(&scheduler, schedule, 0); //this changes depending on how you change the params of schedule

    //the main thread gets a node too; its context is filled in by swapcontext
    mainNode = malloc(sizeof(struct node));
    mainNode->control = calloc(1, sizeof(tcb));
    mainNode->control->threadID = 0;
    mainNode->control->priority = 0;
    mainNode->control->state = SCHEDULED;
    mainNode->control->joinThreadID = -1;
    enqueue(mainNode);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = &timeUp;
    sigaction(SIGPROF, &sa, NULL);

    timer.it_interval.tv_usec = 0; 
    timer.it_interval.tv_sec = 0;

    init_scheduler_done = 1;

    //we start the clock so main gets preempted too
    timer.it_value.tv_usec = QUANTUM;
    timer.it_value.tv_sec = 0;
    setitimer(ITIMER_PROF, &timer, NULL);
}


/* creates a new thread:
* 
* creates a Thread Control Block (TCB)
* allocates space on the stack for the thread to run
* after everything is set, the thread is pushed into the run queue and made ready for execution
*/
int worker_create(worker_t *thread, pthread_attr_t *attr,
                  void *(*function)(void *), void *arg)
{
  
    libEnter(); //ensures that the scheduler context is saved with SIGPROF blocked
    if(init_scheduler_done == 0){
        initialize();
    }

    //allocates space on the stack
    struct node* temp = malloc(sizeof(struct node)); 
    temp->control = calloc(1, sizeof(tcb));
    void* stack = malloc(STACK_SIZE);
    if(temp == NULL || temp->control == NULL || stack == NULL){
        libLeave();
        return -1;
    }

    *thread = nextID;
    temp->control->threadID = *thread;
    nextID++;

    //establishes thread context
    getcontext(&(temp->control->context)); 
    temp->control->context.uc_link=NULL;
    temp->control->context.uc_stack.ss_sp=stack;
    temp->control->context.uc_stack.ss_size=STACK_SIZE;
    temp->control->context.uc_stack.ss_flags=0;
    makecontext(&(temp->control->context), threadEntry, 0);

    //initalizes various thread variables
    temp->control->stack = stack;
    temp->control->fn = function;
    temp->control->arg = arg;
    temp->control->priority = 0;
    temp->control->state = READY; 
    temp->control->joinThreadID = -1;

    enqueue(temp);

    libLeave();
    return 0; //the caller keeps running; the scheduler picks the new thread later
}

/* gives CPU possession to other user-level worker threads voluntarily */
int worker_yield()
{   
    if(init_scheduler_done == 0){
        return 0;
    }
    libEnter();
    head->control->state = READY; //worker's thread state is shifting from RUNNING to READY
    swapcontext(&(head->control->context), &scheduler); //the context goes from the thread to the scheduler
    libLeave();
    return 0;
};

/* terminates a thread */
void worker_exit(void *value_ptr)
{
    libEnter();

    //if the value pointer is provided, we save the return value
    if(value_ptr != NULL){ 
        head->control->retVal = value_ptr;
    }

    //we then wake up anything waiting on us in worker_join
    struct node* curr = head->next;
    while(curr != NULL){ 
        if(curr->control->state == BLOCKED && curr->control->joinThreadID == head->control->threadID){
            curr->control->state = READY;
        }
        curr = curr->next;
    }
    head->control->state = FINISHED; //the scheduler frees the stack and the node stays until joined
    swapcontext(&(head->control->context), &scheduler); //this never returns
}

/* wait for thread termination */
int worker_join(worker_t thread, void **value_ptr)
{
    libEnter();
    struct node* temp = head->next;
    while(temp != NULL){
        if(temp->control->threadID == thread){
            break;
        }
        temp = temp->next;
    }
    if(temp == NULL){ //no such thread (or already joined)
        libLeave();
        return -1;
    }

    while(temp->control->state != FINISHED){
        head->control->joinThreadID = thread;
        head->control->state = BLOCKED;
        swapcontext(&(head->control->context), &scheduler); 
    }
    head->control->joinThreadID = -1;

    //if the value pointer is provided, we retrieve the return value from the joining thread
    if(value_ptr != NULL){ 
        *value_ptr = temp->control->retVal; 
    }
    dequeue(thread); //the thread is taken out of the list and freed
    free(temp->control); //any dynamic memory created by the joinign thread is de-allocated
    free(temp);

    libLeave();
    return 0;
};

/* initialize the mutex lock */
int worker_mutex_init(worker_mutex_t *mutex,
                      const pthread_mutexattr_t *mutexattr)
{
    //initializes data structures for this mutex
    mutex->lock = 0;
    mutex->threadID = -1;
    mutex->blockList = NULL;
    mutex->length = 0;
    return 0;
};

/* aquire the mutex lock */
int worker_mutex_lock(worker_mutex_t *mutex)
{
    libEnter();

    //uses test-and-set atomic function to test the mutex
    while(__sync_lock_test_and_set(&(mutex->lock), 1)){
        head->control->state = BLOCKED;
        if(mutex->blockList == NULL){ //if the mutex is acquired successfully, we enter the critical section
            mutex->blockList = malloc(sizeof(tcb*)); 
            mutex->blockList[mutex->length] = head->control; 
        }
        else{ //otherwise, we push the curent thread into the block list
            mutex->blockList = realloc(mutex->blockList, sizeof(tcb*)*(mutex->length+1));
            mutex->blockList[mutex->length] = head->control;
        }
        mutex->length++;
        swapcontext(&(head->control->context), &scheduler); //context switch to the scheduler
    }

    mutex->threadID = head->control->threadID;
    libLeave();
    return 0;
};

/* release the mutex lock */
int worker_mutex_unlock(worker_mutex_t *mutex)
{
    libEnter();

    //we ready threads to compete for the mutex again later
    for(int i = 0; i < mutex->length; i++){ 
        mutex->blockList[i]->state = READY; //blocked threads never leave the list, so we only need to flip the state
    }

    //we release the mutex and make it available again
    free(mutex->blockList); 
    mutex->blockList = NULL;
    mutex->length = 0;
    __sync_lock_release(&(mutex->lock));
    mutex->threadID = -1;
    libLeave();
    return 0;
};

/* destroy the mutex */
int worker_mutex_destroy(worker_mutex_t *mutex)
{
    //making sure the mutex is not being used
    if(mutex->lock != 0 || mutex->length != 0){
        return -1;
    }

    //we de-allocate dynamic memory created in the mutex init function
    free(mutex->blockList); 
    mutex->blockList = NULL;
    return 0;
};

/* scheduler */
static void schedule()
{
    //every time a timer interrupt occurs, the worker thread library is context switched from a thread context to this

    //we first deal with the thread's state, then pick the next thread
    while(1){
        if(head->control->state == FINISHED){
            free(head->control->stack); //free any memory we no longer need
            head->control->stack = NULL;
        }
        else if(head->control->state == SCHEDULED){ //preempted by the timer
            head->control->state = READY;
        }
        sched_rr(); //get the next thread
    }
}

/*
* round robin scheduler in which we rotate the thread that just ran to the back
* and keep rotating until the front of the list is READY
*/
static void sched_rr()
{
    int count = 0;
    for(struct node* n = head; n != NULL; n = n->next){
        count++;
    }

    //finding the next ready thread
    int found = 0;
    for(int i = 0; i < count; i++){
        if(head->next != NULL){
            enqueue(dequeue(head->control->threadID));  
        }
        if(head->control->state == READY){
            found = 1;
            break;
        }
    }

    if(!found){ //no thread can be run; if the main node is finished, we can exit normally, otherwise deadlock has occurred
        if(mainNode->control->state == FINISHED){ 
            exit(0);
        }
        fprintf(stderr, "worker library: deadlock, no runnable threads\n");
        exit(1);
    }

    //starting the timer for the selected thread, then swapping the context
    timer.it_value.tv_usec = QUANTUM; 
    timer.it_value.tv_sec = 0;
    setitimer(ITIMER_PROF, &timer, NULL);
    head->control->state = SCHEDULED;
    swapcontext(&scheduler, &(head->control->context));
}
