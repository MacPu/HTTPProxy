//
//  LASemaphore.c
//  HTTPProxy
//
//  Created by MacPu on 2016/12/29.
//  Copyright © 2016年 MacPu. All rights reserved.
//

#include "LASemaphore.h"
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

typedef struct semaphore {
    pthread_cond_t   cond;
    pthread_mutex_t mutex;
    int value;
} semaphore_t;


/**************************** headers *******************************/

static int la_semaphore_init(semaphore_t *semaphore, int value);
static int la_semaphore_reset(semaphore_t *semaphore, int value);
static void la_semaphore_signal(semaphore_t *semaphore);
static void la_semaphore_signal_all(semaphore_t *semaphore);
static void la_semaphore_wait(semaphore_t *semaphore);
static int la_semaphore_timedwait(semaphore_t *semaphore, long sec);



LASemaphoreRef * LASemaphoreCreate( int value)
{
    semaphore_t *semaphore = (semaphore_t *)malloc(sizeof(semaphore_t));
    la_semaphore_init(semaphore, value);
    return semaphore;
}

int LASemaphoreReset(LASemaphoreRef *semaphore, int value)
{
    return la_semaphore_reset(semaphore, value);
}

void LASemaphoreSignal(LASemaphoreRef *semaphore)
{
    la_semaphore_signal(semaphore);
}

void LASemaphoreSignalAll(LASemaphoreRef *semaphore)
{
    la_semaphore_signal_all(semaphore);
}

void LASemaphoreWait(LASemaphoreRef *semaphore)
{
    la_semaphore_wait(semaphore);
}

int LASemaphoreTimedWait(LASemaphoreRef *semaphore, long sec)
{
    return la_semaphore_timedwait(semaphore,sec);
}

int LASemaphoreDestroy(LASemaphoreRef *semaphore)
{
    int res = 0;
    res = pthread_mutex_destroy(&(semaphore->mutex));
    if(res != 0){
        return res;
    }
    res = pthread_cond_destroy(&(semaphore->cond));
    if(res != 0){
        return res;
    }
    free(semaphore);
    semaphore = NULL;
    return res;
}

/**************************** static function *******************************/

static int la_semaphore_init(semaphore_t *semaphore, int value)
{
    if(value < 0){
        fprintf(stderr, "semaphore_init():semaphore can take only value >= 0\n");
        printf("semaphore_init():semaphore can take only value >= 0\n");
        return -1;
    }
    pthread_mutex_init(&(semaphore->mutex), NULL);
    pthread_cond_init(&(semaphore->cond), NULL);
    
    semaphore->value = value;
    return 0;
}

static int la_semaphore_reset(semaphore_t *semaphore, int value)
{
    return la_semaphore_init(semaphore, value);
}

static void la_semaphore_signal(semaphore_t *semaphore)
{
    pthread_mutex_lock(&(semaphore->mutex));
    semaphore->value ++;
    pthread_cond_signal(&(semaphore->cond));
    pthread_mutex_unlock(&(semaphore->mutex));
}

static void la_semaphore_signal_all(semaphore_t *semaphore)
{
    pthread_mutex_lock(&(semaphore->mutex));
    semaphore->value = 0;
    pthread_cond_broadcast(&(semaphore->cond));
    pthread_mutex_unlock(&(semaphore->mutex));
}

static void la_semaphore_wait(semaphore_t *semaphore)
{
    pthread_mutex_lock(&(semaphore->mutex));
    if(semaphore->value == 0){
        pthread_cond_wait(&(semaphore->cond), &(semaphore->mutex));
    }
    semaphore->value --;
    pthread_mutex_unlock(&(semaphore->mutex));
}

static int la_semaphore_timedwait(semaphore_t *semaphore, long sec)
{
    pthread_mutex_lock(&(semaphore->mutex));
    int res = 0;
    if(semaphore->value == 0){
        struct timespec tv;
        clock_gettime(CLOCK_REALTIME, &tv);
        tv.tv_sec += sec;
        res = pthread_cond_timedwait(&(semaphore->cond), &(semaphore->mutex), &tv);
    }
    
    semaphore->value --;
    pthread_mutex_unlock(&(semaphore->mutex));
    return res;
}
