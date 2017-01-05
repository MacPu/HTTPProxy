//
//  LAThreadPool.c
//  HTTPProxy
//
//  Created by MacPu on 2016/12/28.
//  Copyright © 2016年 MacPu. All rights reserved.
//

#include "LAThreadPool.h"
#include "LASemaphore.h"
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

static volatile int thpool_keepalive;
static volatile int thpool_on_hold;

/** thread job, */
typedef struct job {
    struct job *next;
    void (*function)(void* arg);
    void *arg;
}job_t;

/** job queue  */
typedef struct jobqueue {
    pthread_mutex_t rwmutex;
    job_t *front;
    job_t *rear;
    LASemaphoreRef *has_jobs;
    int len;
}jobqueue_t;

/** thread  */
typedef struct thread {
    int id;
    pthread_t pthread;
    struct thpool *thpool;
}thread_t;

/** thread pool */
typedef struct thpool {
    thread_t **threads;
    volatile int num_threads_alive;
    volatile int num_threads_working;
    pthread_mutex_t  thcount_lock;
    pthread_cond_t  threads_all_idle;
    jobqueue_t*  job_queue;
}thpool_t;



/**************************** headers *******************************/

static thpool_t * la_thpool_init(int max_num_threads);
static int la_thpool_add_job(thpool_t* thpool, void (*function)(void*), void* arg);
static void la_thpool_wait(thpool_t *thpool);
static void la_thpool_destory(thpool_t *thpool);
static void la_thpool_resume(thpool_t* thpool);
static void la_thpool_pause(thpool_t* thpool);

static thread_t * la_thread_init(thpool_t *thpool, int id);
static void * la_thread_do(thread_t *thread);
static void la_thread_destroy(thread_t *thread);
static void la_thread_hold();

static jobqueue_t * la_jobqueue_init();
static void la_jobqueue_push(jobqueue_t *queue, job_t *job);
static job_t * la_jobqueue_pull(jobqueue_t *queue);
static void la_jobqueue_clear(jobqueue_t *queue);
static void la_jobqueue_destroy(jobqueue_t *queue);


LAThreadPoolRef * LAThreadPoolCreate(int maxNumThread)
{
    return la_thpool_init(maxNumThread);
}

int LAThreadPoolAddJob(LAThreadPoolRef *pool,void (*function)(void*), void* arg)
{
    return la_thpool_add_job(pool, function, arg);
}

void LAThreadPoolWait(LAThreadPoolRef *pool)
{
    la_thpool_wait(pool);
}

void LAThreadPoolResume(LAThreadPoolRef *pool)
{
    la_thpool_resume(pool);
}

void LAThreadPoolPause(LAThreadPoolRef *pool)
{
    la_thpool_pause(pool);
}

void LAThreadPoolDestroy(LAThreadPoolRef *pool)
{
    la_thpool_destory(pool);
}

/**************************** thpool *******************************/

static thpool_t * la_thpool_init(int max_num_threads)
{
    thpool_on_hold = 0;
    thpool_keepalive = 1;
    
    if(max_num_threads< 0){
        max_num_threads = 0;
    }
    thpool_t *thpool = (thpool_t *)malloc(sizeof(thpool_t));
    if(thpool == NULL){
        fprintf(stderr, "la_thpool_init():Could not allocate memory for thread pool\n");
        return NULL;
    }
    
    thpool->num_threads_alive = 0;
    thpool->num_threads_working = 0;
    pthread_cond_init(&(thpool->threads_all_idle), NULL);
    pthread_mutex_init(&(thpool->thcount_lock), NULL);
    
    thpool->job_queue = la_jobqueue_init();
    if(thpool->job_queue == NULL){
        fprintf(stderr, "la_thpool_init(): Could not allocate memory for job queue\n");
        free(thpool);
        return NULL;
    }
    
    thread_t *threads[max_num_threads];
    for(int n = 0 ; n< max_num_threads;n++){
        thread_t *thead = la_thread_init(thpool,n);
        if(thead == NULL){
            //if failed. destory thread that already allocated.
            fprintf(stderr, "la_thpool_init():Could not allocate memory for thread id:%d\n",n);
            for(int i = 0;i < n;i ++){
                la_thread_destroy(threads[i]);
            }
            return NULL;
        }
        threads[n] = thead;
    }
    
    thpool->threads = threads;
    
    while (thpool->num_threads_alive != max_num_threads) { usleep(500000);}
    
    return thpool;
}

static int la_thpool_add_job(thpool_t* thpool, void (*function)(void*), void* arg)
{
//    printf("pool alive:%d working:%d queue:%d \n",thpool->num_threads_alive,thpool->num_threads_working,thpool->job_queue->len);
    
    job_t *newjob = (job_t *)malloc(sizeof(job_t));
    if(newjob == NULL){
        fprintf(stderr, "la_thpool_add_job():Could not allocate memory for job\n");
        return -1;
    }
    newjob->function = function;
    newjob->arg = arg;
    
    pthread_mutex_lock(&(thpool->job_queue->rwmutex));
    la_jobqueue_push(thpool->job_queue, newjob);
    pthread_mutex_unlock(&(thpool->job_queue->rwmutex));
    
    return 0;
}

static void la_thpool_wait(thpool_t *thpool)
{
    pthread_mutex_lock(&(thpool->thcount_lock));
    while (thpool->num_threads_working || thpool->job_queue->len) {
        pthread_cond_wait(&(thpool->threads_all_idle), &(thpool->thcount_lock));
    }
    pthread_mutex_unlock(&(thpool->thcount_lock));
}

static void la_thpool_destory(thpool_t *thpool)
{
    if(thpool == NULL) return;
    
    volatile int threads_total = thpool->num_threads_alive;
    
    double TIMEOUT = 3.0;
    time_t start, end;
    double tpassed = 0.0;
    time (&start);
    
    thpool_keepalive = 0;
    
    /* Give 3 seconds to kill idle threads */
    while (tpassed < TIMEOUT && thpool->num_threads_alive){
        LASemaphoreSignalAll(thpool->job_queue->has_jobs);
        time (&end);
        tpassed = difftime(end,start);
    }
    
    while (thpool -> num_threads_alive) {
        LASemaphoreSignalAll(thpool->job_queue->has_jobs);
        usleep(1);
    }
    
    la_jobqueue_destroy(thpool->job_queue);
    
    for(int i = 0;i<threads_total;i++){
        la_thread_destroy(thpool->threads[i]);
    }
    free(thpool);
}

static void la_thpool_resume(thpool_t* thpool)
{
    thpool_on_hold = 0;
}

static void la_thpool_pause(thpool_t* thpool)
{
    int n;
    for (n=0; n < thpool->num_threads_alive; n++){
        pthread_kill(thpool->threads[n]->pthread, SIGUSR1);
    }
}

/**************************** thread *******************************/

static thread_t * la_thread_init(thpool_t *thpool, int id)
{
    thread_t *thread = (thread_t *)malloc(sizeof(thread_t));
    if(thread == NULL){
        fprintf(stderr, "la_thread_init():Could not alloc memory for thread\n");
    }
    thread->thpool = thpool;
    thread->id = id;
    pthread_create(&(thread->pthread), NULL, (void *)la_thread_do, thread);
    pthread_detach(thread->pthread);
    return thread;
}

static void * la_thread_do(thread_t *thread)
{
    char *thread_name = (char *)malloc(128);
    sprintf(thread_name, "thpool-thread-%d",thread->id);
    
#if defined(__linux__)
    prctl(PR_SET_NAME, thread_name);
#elif defined(__APPLE__) && defined(__MACH__)
    pthread_setname_np(thread_name);
#else
    fprintf(stderr, "la_thread_do(): pthread_setname_np is not supported on this system\n");
#endif
    free(thread_name);
    
    /* Register signal handler */
    struct sigaction act;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = la_thread_hold;
    if (sigaction(SIGUSR1, &act, NULL) == -1) {
        fprintf(stderr, "la_thread_do(): cannot handle SIGUSR1\n");
    }
    
    thpool_t *pool = thread->thpool;
    pthread_mutex_lock(&(pool->thcount_lock));
    pool->num_threads_alive ++;
    pthread_mutex_unlock(&(pool->thcount_lock));
    
    while (thpool_keepalive) {
        LASemaphoreWait(pool->job_queue->has_jobs);
        
        if(thpool_keepalive){
            pthread_mutex_lock(&(pool->thcount_lock));
            pool->num_threads_working ++;
            pthread_mutex_unlock(&(pool->thcount_lock));
            
            void (*func_buff)(void* arg);
            void *arg_buff;
            job_t *job;
            
            pthread_mutex_lock(&(pool->job_queue->rwmutex));
            job = la_jobqueue_pull(pool->job_queue);
            pthread_mutex_unlock(&(pool->job_queue->rwmutex));
            
            if(job && job->arg && job->function){
                func_buff = job->function;
                arg_buff = job->arg;
                func_buff(arg_buff);
            }
            if(job){
                free(job);
            }
            pthread_mutex_lock(&(pool->thcount_lock));
            pool->num_threads_working --;
            if(pool->num_threads_working == 0){
                pthread_cond_signal(&(pool->threads_all_idle));
            }
            pthread_mutex_unlock(&(pool->thcount_lock));
        }
    }
    
    pthread_mutex_lock(&(pool->thcount_lock));
    pool->num_threads_alive --;
    pthread_mutex_unlock(&(pool->thcount_lock));
    
    return NULL;
}

static void la_thread_destroy(thread_t *thread)
{
    pthread_kill(thread->pthread, SIGQUIT);
    free(thread);
}

static void la_thread_hold()
{
    thpool_on_hold = 1;
    while (thpool_on_hold){
        sleep(1);
    }
}

/**************************** job queue *******************************/

static jobqueue_t * la_jobqueue_init()
{
    jobqueue_t *queue = (jobqueue_t *)malloc(sizeof(jobqueue_t));
    if(queue == NULL){
        return NULL;
    }
    queue->len = 0;
    queue->rear = NULL;
    queue->front = NULL;
    
    queue->has_jobs = LASemaphoreCreate(0);
    if(queue->has_jobs == NULL){
        free(queue);
        return NULL;
    }
    
    pthread_mutex_init(&(queue->rwmutex), NULL);
    
    return queue;
}

static void la_jobqueue_push(jobqueue_t *queue, job_t *job)
{
    job->next = NULL;
    if(queue->len == 0){
        queue->front = job;
        queue->rear = job;
    }
    else{
        queue->rear->next = job;
        queue->rear = job;
    }
    
    queue->len ++;
    
    LASemaphoreSignal(queue->has_jobs);
}

static job_t * la_jobqueue_pull(jobqueue_t *queue)
{
    job_t *job = queue->front;
    
    switch (queue->len) {
        case 0:
            break;
        case 1:
            queue->front = NULL;
            queue->rear = NULL;
            queue->len --;
            break;
        default:
            queue->front = job->next;
            queue->len --;
            LASemaphoreSignal(queue->has_jobs);
            break;
    }
    
    return job;
}

static void la_jobqueue_clear(jobqueue_t *queue)
{
    while (queue->len) {
        free(la_jobqueue_pull(queue));
    }
    LASemaphoreReset(queue->has_jobs, 0);
}

static void la_jobqueue_destroy(jobqueue_t *queue)
{
    la_jobqueue_clear(queue);
    LASemaphoreDestroy(queue->has_jobs);
    free(queue);
}
