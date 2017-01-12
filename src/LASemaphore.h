//
//  LASemaphore.h
//  HTTPProxy
//
//  Created by MacPu on 2016/12/29.
//  Copyright © 2016年 MacPu. All rights reserved.
//

#ifndef LASemaphore_h
#define LASemaphore_h

#include <errno.h>

typedef struct semaphore LASemaphoreRef;


/**
 Creates new counting semaphore with an initial value.

 @param value value The starting value for the semaphore. Passing a value less than zero will cause NULL to be returned.
 @return The semaphore. The result of passing NULL when create semaphore failed.
 */
LASemaphoreRef * LASemaphoreCreate( int value);


/**
 reset semaphore with a new value

 @param semaphore The semaphore. The result of passing NULL when reset semaphore failed.
 @param value new value for semaphore.Passing a value less than zero will cause NULL to be returned.
 @return return zero if success. or -1 on failed.
 */
int LASemaphoreReset(LASemaphoreRef *semaphore, int value);


/**
 Signal a semaphore. and also increment the counting semaphore.

 @param semaphore The counting semaphore.
 */
void LASemaphoreSignal(LASemaphoreRef *semaphore);


/**
 Signal all semaphore. and set semaphore value to 0.

 @param semaphore The counting semaphore.
 */
void LASemaphoreSignalAll(LASemaphoreRef *semaphore);


/**
 Wait for a semaphore, and also decrement the counting semaphore.

 @param semaphore The semaphore.
 */
void LASemaphoreWait(LASemaphoreRef *semaphore);


/**
 Wait for a semaphore with timeout, and also decrement the counting semaphore.

 @param semaphore The semaphore.
 @param sec the timeout seconds
 @return return ETIMEDOUT if the timeout occurred.
 */
int LASemaphoreTimedWait(LASemaphoreRef *semaphore, long sec);


/**
 destory and free all memory

 @param semaphore The semaphore.
 @return 0 return if success. or return err code.
 */
int LASemaphoreDestroy(LASemaphoreRef *semaphore);

#endif /* LASemaphore_h */
