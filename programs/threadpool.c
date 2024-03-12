/*
  threadpool.h - part of lz4 project
  Copyright (C) Yann Collet 2023
  GPL v2 License

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along
  with this program; if not, write to the Free Software Foundation, Inc.,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

  You can contact the author at :
  - LZ4 source repository : https://github.com/lz4/lz4
  - LZ4 public forum : https://groups.google.com/forum/#!forum/lz4c
*/


/* ======   Dependencies   ======= */
#include <assert.h>
#include "lz4conf.h"  /* LZ4IO_MULTITHREAD */
#include "threadpool.h"


/* ======   Compiler specifics   ====== */
#if defined(_MSC_VER)
#  pragma warning(disable : 4204)        /* disable: C4204: non-constant aggregate initializer */
#endif


#if !LZ4IO_MULTITHREAD

/* ===================================================== */
/* Backup implementation with no multi-threading support */
/* ===================================================== */

/* Non-zero size, to ensure g_poolCtx != NULL */
struct TPOOL_ctx_s {
    int dummy;
};
static TPOOL_ctx g_poolCtx;

TPOOL_ctx* TPOOL_create(int numThreads, int queueSize) {
    (void)numThreads;
    (void)queueSize;
    return &g_poolCtx;
}

void TPOOL_free(TPOOL_ctx* ctx) {
    assert(!ctx || ctx == &g_poolCtx);
    (void)ctx;
}

void TPOOL_submitJob(TPOOL_ctx* ctx, void (*job_function)(void*), void* arg) {
    (void)ctx;
    job_function(arg);
}

void TPOOL_completeJobs(TPOOL_ctx* ctx) {
    assert(!ctx || ctx == &g_poolCtx);
    (void)ctx;
}

#else

/* pthread only */
#include <stdlib.h>  /* malloc, free*/
#include <pthread.h> /* pthread_* */

/* A job is just a function with an opaque argument */
typedef struct TPOOL_job_s {
    void (*job_function)(void*);
    void *arg;
} TPOOL_job;

struct TPOOL_ctx_s {
    pthread_t* threads;
    size_t threadCapacity;
    size_t threadLimit;

    /* The queue is a circular buffer */
    TPOOL_job* queue;
    size_t queueHead;
    size_t queueTail;
    size_t queueSize;

    /* The number of threads working on jobs */
    size_t numThreadsBusy;
    /* Indicates if the queue is empty */
    int queueEmpty;

    /* The mutex protects the queue */
    pthread_mutex_t queueMutex;
    /* Condition variable for pushers to wait on when the queue is full */
    pthread_cond_t queuePushCond;
    /* Condition variables for poppers to wait on when the queue is empty */
    pthread_cond_t queuePopCond;
    /* Indicates if the queue is shutting down */
    int shutdown;
};

static void TPOOL_shutdown(TPOOL_ctx* ctx);

void TPOOL_free(TPOOL_ctx* ctx) {
    if (!ctx) { return; }
    TPOOL_shutdown(ctx);
    pthread_mutex_destroy(&ctx->queueMutex);
    pthread_cond_destroy(&ctx->queuePushCond);
    pthread_cond_destroy(&ctx->queuePopCond);
    free(ctx->queue);
    free(ctx->threads);
    free(ctx);
}

static void* TPOOL_thread(void* opaque);

TPOOL_ctx* TPOOL_create(int nbThreads, int queueSize)
{
    TPOOL_ctx* ctx;
    /* Check parameters */
    if (nbThreads<1 || queueSize<1) { return NULL; }
    /* Allocate the context and zero initialize */
    ctx = (TPOOL_ctx*)calloc(1, sizeof(TPOOL_ctx));
    if (!ctx) { return NULL; }
    /* init pthread variables */
    {   int error = 0;
        error |= pthread_mutex_init(&ctx->queueMutex, NULL);
        error |= pthread_cond_init(&ctx->queuePushCond, NULL);
        error |= pthread_cond_init(&ctx->queuePopCond, NULL);
        if (error) { TPOOL_free(ctx); return NULL; }
    }
    /* Initialize the job queue.
     * It needs one extra space since one space is wasted to differentiate
     * empty and full queues.
     */
    ctx->queueSize = (size_t)queueSize + 1;
    ctx->queue = (TPOOL_job*)calloc(1, ctx->queueSize * sizeof(TPOOL_job));
    if (ctx->queue == NULL) {
        TPOOL_free(ctx);
        return NULL;
    }
    ctx->queueHead = 0;
    ctx->queueTail = 0;
    ctx->numThreadsBusy = 0;
    ctx->queueEmpty = 1;
    ctx->shutdown = 0;
    /* Allocate space for the thread handles */
    ctx->threads = (pthread_t*)calloc(1, (size_t)nbThreads * sizeof(pthread_t));
    if (ctx->threads == NULL) {
        TPOOL_free(ctx);
        return NULL;
    }
    ctx->threadCapacity = 0;
    /* Initialize the threads */
    {   int i;
        for (i = 0; i < nbThreads; ++i) {
            if (pthread_create(&ctx->threads[i], NULL, &TPOOL_thread, ctx)) {
                ctx->threadCapacity = (size_t)i;
                TPOOL_free(ctx);
                return NULL;
        }   }
        ctx->threadCapacity = (size_t)nbThreads;
        ctx->threadLimit = (size_t)nbThreads;
    }
    return ctx;
}

/* TPOOL_thread() :
 * Work thread for the thread pool.
 * Waits for jobs and executes them.
 * @returns : NULL on failure else non-null.
 */
static void* TPOOL_thread(void* opaque) {
    TPOOL_ctx* const ctx = (TPOOL_ctx*)opaque;
    if (!ctx) { return NULL; }
    for (;;) {
        /* Lock the mutex and wait for a non-empty queue or until shutdown */
        pthread_mutex_lock(&ctx->queueMutex);

        while ( ctx->queueEmpty
            || (ctx->numThreadsBusy >= ctx->threadLimit) ) {
            if (ctx->shutdown) {
                /* even if !queueEmpty, (possible if numThreadsBusy >= threadLimit),
                 * a few threads will be shutdown while !queueEmpty,
                 * but enough threads will remain active to finish the queue */
                pthread_mutex_unlock(&ctx->queueMutex);
                return opaque;
            }
            pthread_cond_wait(&ctx->queuePopCond, &ctx->queueMutex);
        }
        /* Pop a job off the queue */
        {   TPOOL_job const job = ctx->queue[ctx->queueHead];
            ctx->queueHead = (ctx->queueHead + 1) % ctx->queueSize;
            ctx->numThreadsBusy++;
            ctx->queueEmpty = (ctx->queueHead == ctx->queueTail);
            /* Unlock the mutex, signal a pusher, and run the job */
            pthread_cond_signal(&ctx->queuePushCond);
            pthread_mutex_unlock(&ctx->queueMutex);

            job.job_function(job.arg);

            /* If the intended queue size was 0, signal after finishing job */
            pthread_mutex_lock(&ctx->queueMutex);
            ctx->numThreadsBusy--;
            pthread_cond_signal(&ctx->queuePushCond);
            pthread_mutex_unlock(&ctx->queueMutex);
        }
    }  /* for (;;) */
    assert(0);  /* Unreachable */
}

/*! TPOOL_shutdown() :
    Shutdown the queue, wake any sleeping threads, and join all of the threads.
*/
static void TPOOL_shutdown(TPOOL_ctx* ctx) {
    /* Shut down the queue */
    pthread_mutex_lock(&ctx->queueMutex);
    ctx->shutdown = 1;
    pthread_mutex_unlock(&ctx->queueMutex);
    /* Wake up sleeping threads */
    pthread_cond_broadcast(&ctx->queuePushCond);
    pthread_cond_broadcast(&ctx->queuePopCond);
    /* Join all of the threads */
    {   size_t i;
        for (i = 0; i < ctx->threadCapacity; ++i) {
            pthread_join(ctx->threads[i], NULL);  /* note : could fail */
    }   }
}


/*! TPOOL_completeJobs() :
 *  Waits for all queued jobs to finish executing.
 */
void TPOOL_completeJobs(TPOOL_ctx* ctx){
    pthread_mutex_lock(&ctx->queueMutex);
    while(!ctx->queueEmpty || ctx->numThreadsBusy > 0) {
        pthread_cond_wait(&ctx->queuePushCond, &ctx->queueMutex);
    }
    pthread_mutex_unlock(&ctx->queueMutex);
}

/**
 * Returns 1 if the queue is full and 0 otherwise.
 *
 * When queueSize is 1 (pool was created with an intended queueSize of 0),
 * then a queue is empty if there is a thread free _and_ no job is waiting.
 */
static int isQueueFull(TPOOL_ctx const* ctx) {
    if (ctx->queueSize > 1) {
        return ctx->queueHead == ((ctx->queueTail + 1) % ctx->queueSize);
    } else {
        return (ctx->numThreadsBusy == ctx->threadLimit) ||
               !ctx->queueEmpty;
    }
}

static void
TPOOL_submitJob_internal(TPOOL_ctx* ctx, void (*job_function)(void*), void *arg)
{
    TPOOL_job job;
    job.job_function = job_function;
    job.arg = arg;
    assert(ctx != NULL);
    if (ctx->shutdown) return;

    ctx->queueEmpty = 0;
    ctx->queue[ctx->queueTail] = job;
    ctx->queueTail = (ctx->queueTail + 1) % ctx->queueSize;
    pthread_cond_signal(&ctx->queuePopCond);
}

void TPOOL_submitJob(TPOOL_ctx* ctx, void (*job_function)(void*), void* arg)
{
    assert(ctx != NULL);
    pthread_mutex_lock(&ctx->queueMutex);
    /* Wait until there is space in the queue for the new job */
    while (isQueueFull(ctx) && (!ctx->shutdown)) {
        pthread_cond_wait(&ctx->queuePushCond, &ctx->queueMutex);
    }
    TPOOL_submitJob_internal(ctx, job_function, arg);
    pthread_mutex_unlock(&ctx->queueMutex);
}

#endif  /* LZ4IO_NO_MT */
