//
//  LAThreadPool.h
//  HTTPProxy
//
//  Created by MacPu on 2016/12/28.
//  Copyright © 2016年 MacPu. All rights reserved.
//

#ifndef LAThreadPool_h
#define LAThreadPool_h

#include <stdio.h>

typedef struct thpool LAThreadPoolRef;

/**
 create a thread pool.

 @param maxNumThread max thread count
 @return if success will return a LAThreadPoolRef pointer, if failed will return NULL
 */
LAThreadPoolRef * LAThreadPoolCreate(int maxNumThread);


/**
 add a job to the pool. if pool have idle thread, will do this job in idle thread right now.
 if pool have no idle thread. will add job to job queue, and wait idle thread;

 @param pool access to pool
 @param function function of job
 @return zero on success, or non-zero if the timeout occurred.
 */
int LAThreadPoolAddJob(LAThreadPoolRef *pool,void (*function)(void*), void* arg);


/**
 wait for all jobs in pool be done. This will block the current thread

 @param pool wait for pool
 */
void LAThreadPoolWait(LAThreadPoolRef *pool);


/**
 resume the thread pool

 @param pool resume pool
 */
void LAThreadPoolResume(LAThreadPoolRef *pool);


/**
 pause the thread pool

 @param pool pause pool
 */
void LAThreadPoolPause(LAThreadPoolRef *pool);


/**
 destroy the thread pool, and free all resource.

 @param pool destroy pool
 */
void LAThreadPoolDestroy(LAThreadPoolRef *pool);

#endif /* LAThreadPool_h */
