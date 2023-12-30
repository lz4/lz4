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

#ifndef THREADPOOL_H
#define THREADPOOL_H

#if defined (__cplusplus)
extern "C" {
#endif

typedef struct TPOOL_ctx_s TPOOL_ctx;

/*! TPOOL_create() :
 *  Create a thread pool with at most @nbThreads.
 * @nbThreads must be at least 1.
 * @queueSize is the maximum number of pending jobs before blocking.
 * @return : POOL_ctx pointer on success, else NULL.
*/
TPOOL_ctx* TPOOL_create(int nbThreads, int queueSize);

/*! TPOOL_free() :
 *  Free a thread pool returned by TPOOL_create().
 *  Note: if jobs are already running, @free first waits for their completion
 */
void TPOOL_free(TPOOL_ctx* ctx);

/*! TPOOL_submitJob() :
 *  Add @job_function(arg) to the thread pool.
 * @ctx must be valid.
 *  Invocation can block if queue is full.
 *  Note : pay attention to @arg lifetime, which is now owned by @job_function
 */
void TPOOL_submitJob(TPOOL_ctx* ctx, void (*job_function)(void*), void* arg);

/*! TPOOL_completeJobs() :
 *  Blocks, waiting for all queued jobs to be completed
 */
void TPOOL_completeJobs(TPOOL_ctx* ctx);



#if defined (__cplusplus)
}
#endif

#endif /* THREADPOOL_H */
