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
struct TPool_s {
    int dummy;
};
static TPool g_poolCtx;

TPool* TPool_create(int numThreads, int queueSize) {
    (void)numThreads;
    (void)queueSize;
    return &g_poolCtx;
}

void TPool_free(TPool* ctx) {
    assert(!ctx || ctx == &g_poolCtx);
    (void)ctx;
}

void TPool_submitJob(TPool* ctx, void (*job_function)(void*), void* arg) {
    (void)ctx;
    job_function(arg);
}

void TPool_jobsCompleted(TPool* ctx) {
    assert(!ctx || ctx == &g_poolCtx);
    (void)ctx;
}


#elif defined(_WIN32)

/* Window TPool implementation using Completion Ports */
#include <windows.h>

typedef struct TPool_s {
    HANDLE completionPort;
    HANDLE* workerThreads;
    int nbWorkers;
    int queueSize;
    LONG nbPendingJobs;
    HANDLE jobSlotAvail;  /* For queue size control */
    HANDLE allJobsCompleted; /* Event */
} TPool;

void TPool_free(TPool* pool)
{
    if (!pool) return;

    /* Signal workers to exit by posting NULL completions */
    {   int i;
        for (i = 0; i < pool->nbWorkers; i++) {
            PostQueuedCompletionStatus(pool->completionPort, 0, 0, NULL);
        }
    }

    /* Wait for worker threads to finish */
    WaitForMultipleObjects(pool->nbWorkers, pool->workerThreads, TRUE, INFINITE);

    /* Close thread handles and completion port */
    {   int i;
        for (i = 0; i < pool->nbWorkers; i++) {
            CloseHandle(pool->workerThreads[i]);
        }
    }
    free(pool->workerThreads);
    CloseHandle(pool->completionPort);

    /* Clean up synchronization objects */
    CloseHandle(pool->jobSlotAvail);
    CloseHandle(pool->allJobsCompleted);

    free(pool);
}

static DWORD WINAPI WorkerThread(LPVOID lpParameter)
{
    TPool* const pool = (TPool*)lpParameter;
    DWORD bytesTransferred;
    ULONG_PTR completionKey;
    LPOVERLAPPED overlapped;

    while (GetQueuedCompletionStatus(pool->completionPort,
                                    &bytesTransferred, &completionKey,
                                    &overlapped, INFINITE)) {

        /* End signal */
        if (overlapped == NULL) { break; }

        /* Execute job */
        ((void (*)(void*))completionKey)(overlapped);

        /* Signal job completion */
        if (InterlockedDecrement(&pool->nbPendingJobs) == 0) {
            SetEvent(pool->allJobsCompleted);
        }
        ReleaseSemaphore(pool->jobSlotAvail, 1, NULL);
    }

    return 0;
}

TPool* TPool_create(int nbWorkers, int queueSize)
{
    TPool* pool;

    /* parameters sanitization */
    if (nbWorkers <= 0 || queueSize <= 0) return NULL;
    if (nbWorkers>LZ4_NBWORKERS_MAX) nbWorkers=LZ4_NBWORKERS_MAX;

    pool = calloc(1, sizeof(TPool));
    if (!pool) return NULL;

    /* Create completion port */
    pool->completionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, nbWorkers);
    if (!pool->completionPort) { goto _cleanup; }

    /* Create worker threads */
    pool->nbWorkers = nbWorkers;
    pool->workerThreads = (HANDLE*)malloc(sizeof(HANDLE) * nbWorkers);
    if (pool->workerThreads == NULL) { goto _cleanup; }

    {   int i;
        for (i = 0; i < nbWorkers; i++) {
            pool->workerThreads[i] = CreateThread(NULL, 0, WorkerThread, pool, 0, NULL);
            if (!pool->workerThreads[i]) { goto _cleanup; }
        }
    }

    /* Initialize sync objects members */
    pool->queueSize = queueSize;
    pool->nbPendingJobs = 0;

    pool->jobSlotAvail = CreateSemaphore(NULL, queueSize+nbWorkers, queueSize+nbWorkers, NULL);
    if (!pool->jobSlotAvail) { goto _cleanup; }

    pool->allJobsCompleted = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!pool->allJobsCompleted) { goto _cleanup; }

    return pool;

_cleanup:
    TPool_free(pool);
    return NULL;
}


void TPool_submitJob(TPool* pool, void (*job_function)(void*), void* arg)
{
    assert(pool);

    /* Atomically increment pending jobs and check for overflow */
    WaitForSingleObject(pool->jobSlotAvail, INFINITE);
    ResetEvent(pool->allJobsCompleted);
    InterlockedIncrement(&pool->nbPendingJobs);

    /* Post the job directly to the completion port */
    PostQueuedCompletionStatus(pool->completionPort,
                               0, /* Bytes transferred not used */
                               (ULONG_PTR)job_function, /* Store function pointer in completionKey */
                               (LPOVERLAPPED)arg);      /* Store argument in overlapped */
}

void TPool_jobsCompleted(TPool* pool)
{
    assert(pool);
    WaitForSingleObject(pool->allJobsCompleted, INFINITE);
}

#else

/* pthread availability assumed */
#include <stdlib.h>
#include <pthread.h>

typedef struct Job {
    void (*function)(void*);
    void* arg;
} Job;

typedef struct TPool_s {
    pthread_t* threads;
    Job* jobQueue;
    int queueSize;
    int queueCapacity;
    int queueFront;
    int queueBack;
    int nbThreads;
    int stop;

    pthread_mutex_t mutex;
    pthread_cond_t cond_not_empty;
    pthread_cond_t cond_jobs_completed;
    int jobsInProgress;
} TPool;

void* worker_thread(void* arg)
{
    TPool* pool = (TPool*)arg;
    while (1) {
        pthread_mutex_lock(&pool->mutex);

        while (pool->queueSize == 0 && !pool->stop) {
            pthread_cond_wait(&pool->cond_not_empty, &pool->mutex);
        }

        if (pool->stop && pool->queueSize == 0) {
            pthread_mutex_unlock(&pool->mutex);
            break;
        }

        {   Job job = pool->jobQueue[pool->queueFront];
            pool->queueFront = (pool->queueFront + 1) % pool->queueCapacity;
            pool->queueSize--;
            pool->jobsInProgress++;

            pthread_mutex_unlock(&pool->mutex);

            job.function(job.arg);
        }

        pthread_mutex_lock(&pool->mutex);
        pool->jobsInProgress--;
        if (pool->jobsInProgress == 0 && pool->queueSize == 0) {
            pthread_cond_signal(&pool->cond_jobs_completed);
        }
        pthread_mutex_unlock(&pool->mutex);
    }
    return NULL;
}

TPool* TPool_create(int nbThreads, int queueSize)
{
    TPool* pool = (TPool*)malloc(sizeof(TPool));
    if (!pool) return NULL;
    assert(nbThreads >= 1);

    pool->nbThreads = nbThreads;
    pool->queueCapacity = queueSize;
    pool->queueSize = 0;
    pool->queueFront = 0;
    pool->queueBack = 0;
    pool->stop = 0;
    pool->jobsInProgress = 0;

    pool->threads = (pthread_t*)malloc(nbThreads * sizeof(pthread_t));
    pool->jobQueue = (Job*)malloc(queueSize * sizeof(Job));
    if (!pool->threads || !pool->jobQueue) {
        free(pool->threads);
        free(pool->jobQueue);
        free(pool);
        return NULL;
    }

    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->cond_not_empty, NULL);
    pthread_cond_init(&pool->cond_jobs_completed, NULL);

    for (int i = 0; i < nbThreads; i++) {
        pthread_create(&pool->threads[i], NULL, worker_thread, pool);
    }

    return pool;
}

void TPool_submitJob(TPool* ctx, void (*job_function)(void*), void* arg)
{
    pthread_mutex_lock(&ctx->mutex);

    ctx->jobQueue[ctx->queueBack].function = job_function;
    ctx->jobQueue[ctx->queueBack].arg = arg;
    ctx->queueBack = (ctx->queueBack + 1) % ctx->queueCapacity;
    ctx->queueSize++;

    pthread_cond_signal(&ctx->cond_not_empty);
    pthread_mutex_unlock(&ctx->mutex);
}

void TPool_jobsCompleted(TPool* ctx)
{
    pthread_mutex_lock(&ctx->mutex);
    while (ctx->queueSize > 0 || ctx->jobsInProgress > 0) {
        pthread_cond_wait(&ctx->cond_jobs_completed, &ctx->mutex);
    }
    pthread_mutex_unlock(&ctx->mutex);
}

void TPool_free(TPool* ctx)
{
    pthread_mutex_lock(&ctx->mutex);
    ctx->stop = 1;
    pthread_cond_broadcast(&ctx->cond_not_empty);
    pthread_mutex_unlock(&ctx->mutex);

    for (int i = 0; i < ctx->nbThreads; i++) {
        pthread_join(ctx->threads[i], NULL);
    }

    pthread_mutex_destroy(&ctx->mutex);
    pthread_cond_destroy(&ctx->cond_not_empty);
    pthread_cond_destroy(&ctx->cond_jobs_completed);

    free(ctx->threads);
    free(ctx->jobQueue);
    free(ctx);
}

#endif  /* LZ4IO_NO_MT */
