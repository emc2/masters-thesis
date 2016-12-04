/* Copyright (c) 2007, 2008 Eric McCorkle.  All rights reserved. */

#include <stdio.h>

#include "definitions.h"
#include "atomic.h"
#include "gc.h"
#include "mm/mm_malloc.h"
#include "cc/scheduler.h"
#include "cc/executor.h"
#include "cc/thread.h"
#include "cc/lf_thread_queue.h"

static lf_thread_queue_t* sched_workshare;
static volatile atomic_uint_t sched_active_threads;

static inline unsigned int sched_upper_bound(void) {

  const unsigned int num_executors = executor_count();
  const unsigned int num_threads = sched_active_threads.value;
  const unsigned int frac = num_threads >> 2;
  const unsigned int numerator = num_threads + frac;
  const unsigned int bound = numerator / num_executors;

  return bound != 0 ? bound : 1;

}


static inline unsigned int sched_lower_bound(void) {

  const unsigned int num_executors = executor_count();
  const unsigned int num_threads = sched_active_threads.value;
  const unsigned int frac = num_threads >> 2;
  const unsigned int numerator = frac < num_threads ?
    num_threads - frac : 0;
  const unsigned int bound = numerator / num_executors;

  return bound != 0 ? bound : 1;

}


extern unsigned int scheduler_request(const cc_stat_t* const restrict stat) {

  PRINTD("    Reserving space for scheduler system\n");

  const unsigned int execs = stat->cc_num_executors;
  const unsigned int queue_cap = execs * (execs > 16 ? execs : 16);
  const unsigned int workshare_size = lf_thread_queue_request(queue_cap, execs);

  PRINTD("      Reserving 0x%x bytes for thread queue\n", workshare_size);

  return workshare_size;

}


internal void* scheduler_start(const unsigned int execs, void* const mem) {

  INVARIANT(sched_workshare == NULL);

  const unsigned int queue_cap = execs * (execs > 16 ? execs : 16);
  void* ptr = lf_thread_queue_init(mem, queue_cap, execs);

  PRINTD("Starting scheduler system\n");
  sched_workshare = mem;
  sched_active_threads.value = 1;
  PRINTD("Scheduler system is ready\n");

  /* Global invariant: sched_workshare must be initialized in
   * scheduler_start.
   */
  INVARIANT(sched_workshare != NULL);

  return ptr;

}


internal void scheduler_stop(const unsigned int exec) {

  /* Entry invariant: exec is the current executor ID */
  INVARIANT(sched_workshare != NULL);
  /* Entry invariant: call strictly follows scheduler_start */

  thread_t* curr;

  PRINTD("Shutting down scheduler system\n");
  PRINTD("Destroying all threads in workshare\n");
  /* Empty the workshare queue first. */
  while(NULL != (curr = lf_thread_queue_dequeue(sched_workshare, exec))) {

    PRINTD("Destroying thread %p in workshare\n", curr);
    thread_destroy(curr);

  }

  PRINTD("Destroying workshare queue\n");  
  lf_thread_queue_destroy(sched_workshare, exec);
  sched_workshare = NULL;
  PRINTD("Scheduler system offline\n");

  INVARIANT(sched_workshare == NULL);
  /* Global invariant: sched_workshare must be destroyed in
   * scheduler_stop.
   */

}


internal void* scheduler_setup(scheduler_t* const restrict scheduler,
			       void* const mem) {

  return mem;

}


internal void scheduler_init(scheduler_t* const restrict scheduler,
			     thread_t* const restrict idle_thread,
			     thread_t* const restrict gc_thread) {

  /* Entry invariant: call strictly follows scheduler_start */
  /* Entry invariant: call strictly preceeds scheduler_stop */
  INVARIANT(scheduler != NULL);

  PRINTD("Initializing scheduler %p\n", scheduler);
  scheduler->sch_queue_head = NULL;
  scheduler->sch_queue_tail = NULL;
  scheduler->sch_num_threads = 0;
  scheduler->sch_curr_thread = NULL;
  scheduler->sch_idle_thread = idle_thread;
  scheduler->sch_gc_thread = gc_thread;

}


internal void scheduler_destroy(scheduler_t* const restrict scheduler) {

  INVARIANT(scheduler != NULL);
  INVARIANT(scheduler->sch_idle_thread != NULL);

  PRINTD("Destroying scheduler %p\n", scheduler);
  PRINTD("Destroying scheduler %p's idle thread\n", scheduler);
  thread_destroy(scheduler->sch_idle_thread);

  if(NULL != scheduler->sch_curr_thread) {

    PRINTD("Destroying scheduler %p's current thread\n", scheduler);
    thread_destroy(scheduler->sch_curr_thread);

  }

  PRINTD("Destroying all threads in scheduler %p\n", scheduler);

  while(NULL != scheduler->sch_queue_tail) {

    thread_t* const next = scheduler->sch_queue_tail->t_queue_next;

    PRINTD("Destroying thred %p in scheduler %p \n",
	   scheduler->sch_queue_tail, scheduler);
    thread_destroy(scheduler->sch_queue_tail);
    scheduler->sch_queue_tail = next;

  }

}


static inline bool sched_queue_empty(scheduler_t* const restrict scheduler) {

  INVARIANT(scheduler != NULL);
  INVARIANT((scheduler->sch_queue_tail != NULL &&
	     scheduler->sch_queue_head != NULL) ||
	    (scheduler->sch_queue_tail == NULL &&
	     scheduler->sch_queue_head == NULL));

  return scheduler->sch_queue_tail == NULL;

}


/* These are simple local-queue access functions.  They are completely
 * sequential.  Local queues are not shared.  These functions do not
 * affect the reference count in any way
 */
static inline void sched_queue_enqueue(scheduler_t* const restrict scheduler,
				       thread_t* const restrict thread,
				       const unsigned int exec) {

  INVARIANT(scheduler != NULL);
  INVARIANT((scheduler->sch_queue_tail != NULL &&
	     scheduler->sch_queue_head != NULL) ||
	    (scheduler->sch_queue_tail == NULL &&
	     scheduler->sch_queue_head == NULL));
  INVARIANT(scheduler->sch_queue_tail == NULL ||
	    scheduler->sch_queue_tail->t_queue_next == NULL);
  INVARIANT(thread != NULL);
  INVARIANT(thread->t_sched_stat_ref.value & T_REF);

  PRINTD("Putting a thread on the local queue\n");
  thread->t_queue_next = NULL;
  scheduler->sch_num_threads++;

  if(NULL != scheduler->sch_queue_tail) {

    scheduler->sch_queue_tail->t_queue_next = thread;
    scheduler->sch_queue_tail = thread;

  }

  else {

    scheduler->sch_queue_tail = thread;
    scheduler->sch_queue_head = thread;

  }

}


static inline thread_t* sched_queue_dequeue(scheduler_t* const
					    restrict scheduler) {

  INVARIANT(scheduler != NULL);
  INVARIANT(scheduler->sch_queue_tail != NULL);
  INVARIANT(scheduler->sch_queue_head != NULL);
  INVARIANT(scheduler->sch_queue_tail->t_queue_next == NULL);

  PRINTD("Taking a thread from the local queue\n");

  thread_t* const out = scheduler->sch_queue_head;

  INVARIANT(out->t_sched_stat_ref.value & T_REF);

  scheduler->sch_num_threads--;

  if(scheduler->sch_queue_head != scheduler->sch_queue_tail) {

    PRINTD("Out: %p, next: %p\n", out, out->t_queue_next);
    scheduler->sch_queue_head = out->t_queue_next;

  }

  else {

    PRINTD("Took the last thread from the queue\n");
    scheduler->sch_queue_tail = NULL;
    scheduler->sch_queue_head = NULL;

  }

  return out;

}


static inline int scheduler_try_take_thread(scheduler_t* const
					    restrict scheduler,
					    thread_t* const restrict thread,
					    const unsigned int exec) {

  INVARIANT(sched_workshare != NULL);

  const unsigned int oldvalue = thread->t_sched_stat_ref.value;
  const thread_sched_stat_t oldstat =
    (thread_sched_stat_t)(oldvalue & T_STAT_MASK);
  bool out = 1;

  INVARIANT(oldvalue & T_REF);

  /* If it's observed to be runnable, then put it in the local queue
   * and don't mess with sched_stat_ref
   */
  if(T_STAT_RUNNABLE == oldstat || T_STAT_RUNNING == oldstat ||
     T_STAT_GC_WAIT == oldstat || T_STAT_FINALIZER_LIVE == oldstat)
    sched_queue_enqueue(scheduler, thread, exec);

  /* If it's destroyed, then I have the only one, throw it out */
  else if(T_STAT_DESTROY == oldstat) {

    PRINTD("Destroying thread %p\n", thread);
    thread_destroy(thread);
    out = -1;

  }

  /* Otherwise, unset the reference, and keep the status as it is, and
   * return the code for failure
   */
  else {

    if(atomic_compare_and_set_uint(oldvalue, oldstat,
				   &(thread->t_sched_stat_ref)))
      out = -1;

    else
      out = 0;

  }

  return out;

}


/* Try to take one thread from the workshare queue and add it to this
 * scheduler.
 */
static inline void scheduler_take_thread(scheduler_t* const restrict scheduler,
					 const unsigned int exec) {

  INVARIANT(sched_workshare != NULL);
  INVARIANT(scheduler != NULL);

  PRINTD("Executor %u taking a thread from the workshare\n", exec);

  thread_t* thread;

  /* Try until a successful dequeue occurs, or the queue goes empty */
  while(NULL != (thread = lf_thread_queue_dequeue(sched_workshare, exec))) {

    int res;

    for(unsigned int i = 0;
	!(res = scheduler_try_take_thread(scheduler, thread, exec));
	i++)
      backoff_delay(i);

    if(0 < res)
      break;

  }

}


static inline int scheduler_try_put_thread(thread_t* const restrict thread,
					   const unsigned int exec) {

  INVARIANT(sched_workshare != NULL);

  /* Do not decrement the reference count, instead, check it against
   * 1, in case someone else yanks it out from under my feet.
   */
  const unsigned int oldvalue = thread->t_sched_stat_ref.value;
  const thread_sched_stat_t oldstat =
    (thread_sched_stat_t)(oldvalue & T_STAT_MASK);
  bool out = 1;

  INVARIANT(oldvalue & T_REF);

  /* If it's runnable, put it in the lock-free queue, and don't touch
   * sched_stat_ref.
   */
  if(T_STAT_RUNNABLE == oldstat || T_STAT_RUNNING == oldstat ||
     T_STAT_GC_WAIT == oldstat || T_STAT_FINALIZER_LIVE == oldstat)
    lf_thread_queue_enqueue(sched_workshare, thread, exec);

  /* If it's destroyed, then I have the only one, throw it out */
  else if(T_STAT_DESTROY == oldstat) {

    PRINTD("Destroying thread %p\n", thread);
    thread_destroy(thread);
    out = -1;

  }

  /* Otherwise, unset the reference flag, and drop the thread; someone
   * else has a pointer somewhere.  Return the code for failure.
   */
  else {

    if(atomic_compare_and_set_uint(oldvalue, oldstat,
				   &(thread->t_sched_stat_ref)))
      out = -1;

    else
      out = 0;

  }

  return out;

}


/* Put a thread from this scheduler on the workshare queue */
static inline void scheduler_put_thread(scheduler_t* const restrict scheduler,
					const unsigned int exec) {

  INVARIANT(sched_workshare != NULL);
  INVARIANT(scheduler != NULL);
  INVARIANT(scheduler->sch_queue_tail != NULL);
  INVARIANT(scheduler->sch_queue_head != NULL);

  PRINTD("Executor %u putting a thread on the workshare\n", exec);

  thread_t* thread;

  /* Try until a successful dequeue occurs, or the queue goes empty */
  while(NULL != (thread = sched_queue_dequeue(scheduler))) {

    int res;

    for(unsigned int i = 0;
	!(res = scheduler_try_put_thread(thread, exec));
	i++)
      backoff_delay(i);

    if(0 < res)
      break;

  }

}


/* This function may end up destroying threads */

static inline void scheduler_balance_threads(scheduler_t* const
					     restrict scheduler,
					     const unsigned int exec) {

  const unsigned int num_executors = executor_count();
  const unsigned int num_threads = sched_active_threads.value;

  PRINTD("Executor %u balancing threads (has %u threads)\n",
	 exec, scheduler->sch_num_threads);

  if(num_threads > num_executors) {

    if(scheduler->sch_num_threads < sched_lower_bound())
      scheduler_take_thread(scheduler, exec);

    else if(scheduler->sch_num_threads > sched_upper_bound())
      scheduler_put_thread(scheduler, exec);

  }

  else if(0 == scheduler->sch_num_threads)
    scheduler_take_thread(scheduler, exec);

}

/* Try to set the status to some acknowledgement status (ie RUNNING,
 * SUSPENDED, or DEAD).  This function may destroy the thread.  This
 * assumes that if a thread is set to RUNNING, it will be executed
 * immediately, and has just been pulled from a queue.
 *
 * This will return EXECUTE, SKIP, or DISCARD.
 */
typedef enum {
  EXECUTE,
  SKIP,
  DISCARD
} action_t;

static inline action_t
sched_set_running_status(thread_t* const restrict thread) {

  bool cont = true;
  action_t out = DISCARD;

  PRINTD("Acknowledging thread %p's status\n", thread);

  for(unsigned int i = 0; cont; i++) {

    const unsigned int oldvalue = thread->t_sched_stat_ref.value;
    const thread_sched_stat_t oldstat =
      (thread_sched_stat_t)(oldvalue & T_STAT_MASK);

    INVARIANT(oldvalue & T_REF);
    INVARIANT(oldstat != T_STAT_DEAD);
    INVARIANT(oldstat != T_STAT_SUSPENDED);
    INVARIANT(oldstat == T_STAT_RUNNING ||
	      oldstat == T_STAT_RUNNABLE ||
	      oldstat == T_STAT_SUSPEND ||
	      oldstat == T_STAT_TERM ||
	      oldstat == T_STAT_DESTROY ||
	      oldstat == T_STAT_GC_WAIT ||
	      oldstat == T_STAT_FINALIZER_LIVE);

    PRINTD("Thread %p's status is %x\n", thread, oldstat);

    switch(oldstat) {

      /* If it's running, leave the reference flag set */
    case T_STAT_RUNNING:
    case T_STAT_RUNNABLE:
    case T_STAT_FINALIZER_LIVE:

      PRINTD("Setting thread %p's status to running\n", thread);

      if(atomic_compare_and_set_uint(oldvalue, T_STAT_RUNNING | T_REF,
				     &(thread->t_sched_stat_ref))) {

	cont = false;
	out = EXECUTE;

      }

      else
	backoff_delay(i);

      break;

      /* For the gc_wait state, if the collection is running, leave it
       * alone, otherwise, set to T_STAT_RUNNING, just as above.
       */
    case T_STAT_GC_WAIT:

      if(GC_STATE_INACTIVE == (gc_state.value & GC_STATE_PHASE)) {

	PRINTD("Setting thread %p's status to running\n", thread);

	if(atomic_compare_and_set_uint(oldvalue, T_STAT_RUNNING | T_REF,
				       &(thread->t_sched_stat_ref))) {

	  cont = false;
	  out = EXECUTE;

	}

	else
	  backoff_delay(i);

      }

      else {

	out = SKIP;
	cont = false;

      }

      break;

      /* For all unrunnable states, clear the reference flag and drop
       * the thread.  Someone else has it somewhere.
       */
    case T_STAT_SUSPEND:

      PRINTD("Setting thread %p's status to suspended\n", thread);
      if(cont =
	 !atomic_compare_and_set_uint(oldvalue, T_STAT_SUSPENDED,
				      &(thread->t_sched_stat_ref)))
	backoff_delay(i);

      break;

    case T_STAT_TERM:

      PRINTD("Setting thread %p's status to dead\n", thread);
      if(cont =
	 !atomic_compare_and_set_uint(oldvalue, T_STAT_DEAD,
				       &(thread->t_sched_stat_ref)))
	backoff_delay(i);

      break;

      /* If it's destroyed, this is the only copy, so kill it off */
    case T_STAT_DESTROY:

      cont = false;
      thread_destroy(thread);
      break;

      /* Error conditions disallowed by invariants. */

    case T_STAT_SUSPENDED:
    case T_STAT_DEAD:

      panic("Thread %p's status was acknowledged and non-runnable, "
	    "should not happen!\n", thread);
      break;

    case T_STAT_FINALIZER_WAIT:

      panic("Thread %p's status was finalizer waiting, "
	      "should not happen!\n", thread);

      break;

    default:

      panic("Thread %p's status was other, should not happen!\n", thread);

      break;

    }

  }

  return out;

}


static inline void sched_try_workshare(scheduler_t* const restrict sched,
				       const unsigned int exec) {

  while(NULL == sched->sch_curr_thread) {

    PRINTD("Executor %u trying to dequeue from workshare\n", exec);

    thread_t* const thread = lf_thread_queue_dequeue(sched_workshare, exec);

    if(NULL != thread) {

      PRINTD("Executor %u got a thread\n", exec);

      /* If the thread is runnable, take it, otherwise, discard and
       * possibly destroy the thread.
       */
      if(DISCARD != sched_set_running_status(thread)) {

	PRINTD("Executor %u got a runnable thread\n", exec);
	sched->sch_curr_thread = thread;
	sched->sch_num_threads++;
	scheduler_balance_threads(sched, exec);

      }

    }

    else {

      PRINTD("Executor %u failed to dequeue from workshare, "
	     "there are %u active threads\n", exec,
	     sched_active_threads.value);
      break;

    }

  }

}


/* If current thread is null, then you get either idle or GC,
 * depending on things.
 */
static inline thread_t* scheduler_result(const scheduler_t* const
					 restrict scheduler) {

  thread_t* out = scheduler->sch_curr_thread;

  if(NULL == out) {

    if(GC_STATE_INACTIVE == gc_state.value & GC_STATE_PHASE)
      out = scheduler->sch_idle_thread;

    else
      out = scheduler->sch_gc_thread;

  }

  return out;

}

internal thread_t* scheduler_cycle(scheduler_t* const restrict scheduler,
				   const unsigned int exec) {

  INVARIANT(scheduler != NULL);

  PRINTD("Executor %u entering scheduler function\n", exec);

  /* Easy case: there is a current thread. */
  if(NULL != scheduler->sch_curr_thread) {

    const unsigned int state = gc_state.value & GC_STATE_PHASE;

    PRINTD("Executor %u has a current thread\n", exec);
    /* Easy case: the current thread is runnable */
    if(thread != exec->ex_scheduler.sch_gc_thread) {

      if(EXECUTE == sched_set_running_status(scheduler->sch_curr_thread)) {

	PRINTD("Executor %u has a runnable current thread\n", exec);
	scheduler_balance_threads(scheduler, exec);

      }

      /* Otherwise replace it, discarding and possibly destroying it */
      else
	scheduler_replace(scheduler, exec);

    }

    else if(GC_STATE_INACTIVE != gc_state.value) {

      PRINTD("Executor %u was running the garbage collector, "
	     "and collection is underway\n", exec);
      scheduler_balance_threads(scheduler, exec);

    }

    else {

      PRINTD("Executor %u was running the garbage collector, "
	     "and collection is not running\n", exec);
      scheduler_replace(scheduler, exec);

    }

  }

  /* Otherwise try to get a thread from workshare */
  sched_try_workshare(scheduler, exec);
  PRINTD("Executor %u's scheduler returned thread %p\n",
	 exec, scheduler->sch_curr_thread);
  PRINTD("Executor %u now has %u threads\n",
	 exec, scheduler->sch_num_threads);

  return scheduler_result(scheduler);

}


internal thread_t* scheduler_replace(scheduler_t* const restrict scheduler,
				     const unsigned int exec) {

  INVARIANT(scheduler != NULL);
  INVARIANT(scheduler->sch_curr_thread != NULL);

  PRINTD("Executor %u replacing its current thread\n", exec);
  scheduler->sch_curr_thread = NULL;
  scheduler->sch_num_threads--;

  /* Try finding a runnable thread in the local queue */
  while(!sched_queue_empty(scheduler) && NULL == scheduler->sch_curr_thread) {

    PRINTD("Executor %u trying local dequeue\n", exec);
    thread_t* const thread = sched_queue_dequeue(scheduler);

    /* If the thread is runnable, take it, otherwise, discard and
     * possibly destroy it
     */
    if(EXECUTE == sched_set_running_status(thread)) {

      PRINTD("Executor %u found a runnable thread\n", exec);
      scheduler->sch_curr_thread = thread;
      scheduler_balance_threads(scheduler, exec);

    }

  }

  /* Otherwise try to get a thread from workshare */
  sched_try_workshare(scheduler, exec);
  PRINTD("Executor %u now has %u threads\n",
	 exec, scheduler->sch_num_threads);

  return scheduler_result(scheduler);

}


static inline int scheduler_try_activate_thread(thread_t* const thread,
						const unsigned int exec) {

  const unsigned int oldvalue = thread->t_sched_stat_ref.value;
  const thread_sched_stat_t oldstat =
    (thread_sched_stat_t)(oldvalue & T_STAT_MASK);
  const unsigned int ref = oldvalue & T_REF;
  int out;

  PRINTD("Executor %u trying to activate thread %p\n", exec, thread);

  if(T_STAT_TERM != oldstat && T_STAT_DEAD != oldstat &&
     T_STAT_DESTROY != oldstat) {

    /* If the thread is not referenced, put it in the workshare and
     * set the reference flag
     */
    if(!ref) {

      if(out = atomic_compare_and_set_uint(oldvalue, T_STAT_RUNNABLE | T_REF,
					   &(thread->t_sched_stat_ref))) {

	PRINTD("Thread %p was not referenced, inserting\n", thread);

	if(T_STAT_RUNNABLE != oldstat && T_STAT_RUNNING != oldstat &&
	   T_STAT_GC_WAIT != oldstat)
	  atomic_increment_uint(&sched_active_threads);

	lf_thread_queue_enqueue(sched_workshare, thread, exec);

	/* XXX don't do this here.  Wake up executors only when the
	 * queue gets too full and some are sleeping.
	 */
	executor_restart_idle();

      }

    }

    /* Otherwise, just set its status, and someone will pick it up */
    else {

      if(out = atomic_compare_and_set_uint(oldvalue, T_STAT_RUNNABLE | T_REF,
					   &(thread->t_sched_stat_ref)))
	if(T_STAT_RUNNABLE != oldstat && T_STAT_RUNNING != oldstat &&
	   T_STAT_GC_WAIT != oldstat)
	  atomic_increment_uint(&sched_active_threads);

    }

  }

  else
    out = -1;

  return out;

}


internal bool scheduler_activate_thread(thread_t* const thread,
					const unsigned int exec) {

  INVARIANT(thread != NULL);

  unused int res;

  PRINTD("Executor %u activating thread %p\n", exec, thread);

  for(unsigned int i = 0;
      !(res = scheduler_try_activate_thread(thread, exec));
      i++)
    backoff_delay(i);

  return 0 < res;

}


static inline int scheduler_try_deactivate_thread(thread_t* const thread,
						  const thread_sched_stat_t
						  stat,
						  const unsigned int exec) {

  INVARIANT(thread != NULL);
  INVARIANT(stat == T_STAT_SUSPEND || stat == T_STAT_TERM ||
	    stat == T_STAT_DESTROY || stat == T_STAT_GC_WAIT);

  volatile unsigned int* const executor_ptr =
    thread_mbox_executor(thread->t_mbox);
  const unsigned int executor = *executor_ptr;
  const unsigned int oldvalue = thread->t_sched_stat_ref.value;
  const thread_sched_stat_t oldstat =
    (thread_sched_stat_t)(oldvalue & T_STAT_MASK);
  const unsigned int ref = oldvalue & T_REF;
  int out;

  INVARIANT(oldstat != T_STAT_DESTROY);
  PRINTD("Executor %u trying to deactivate thread %p\n", exec, thread);

  if((T_STAT_DEAD != oldstat && T_STAT_TERM != oldstat) ||
     T_STAT_DESTROY == stat) {

    if(out = atomic_compare_and_set_uint(oldvalue, stat | ref,
					 &(thread->t_sched_stat_ref))) {

      /* If the thread has been marked destroyed, and has no
       * references, then destroy it.
       */
      if(T_STAT_DESTROY == stat && 0 == ref) {

	PRINTD("Destroying thread %p\n", thread);
	thread_destroy(thread);

      }

      /* Decrement the active threads and signal the executor */
      if(T_STAT_RUNNABLE == oldstat || T_STAT_RUNNING == oldstat)
	atomic_decrement_uint(&sched_active_threads);

      if(executor != thread_mbox_null_executor) {

	if(executor != exec)
	  executor_raise(executor, EX_SIGNAL_SCHEDULE);

	else {

	  PRINTD("Executor %u deactivating its current thread...  "
		 "rescheduling\n",
		 exec);
	  cc_sched_cycle(exec);

	}

      }

    }

  }

  else
    out = -1;

  return out;

}


internal bool scheduler_deactivate_thread(thread_t* const thread,
					  const thread_sched_stat_t stat,
					  const unsigned int exec) {

  INVARIANT(thread != NULL);
  INVARIANT(stat == T_STAT_SUSPEND || stat == T_STAT_TERM ||
	    stat == T_STAT_DESTROY || stat == T_STAT_GC_WAIT);

  unused int res;

  PRINTD("Executor %u deactivatng thread %p\n", exec, thread);

  for(unsigned int i = 0;
      !(res = scheduler_try_deactivate_thread(thread, stat, exec));
      i++)
    backoff_delay(i);

  return 0 < res;

}


static inline int scheduler_try_update_thread(thread_t* const thread,
					      const thread_sched_stat_t stat) {

  INVARIANT(thread != NULL);
  INVARIANT(stat == T_STAT_SUSPEND || stat == T_STAT_TERM ||
	    stat == T_STAT_DESTROY || stat == T_STAT_GC_WAIT);

  const unsigned int oldvalue = thread->t_sched_stat_ref.value;
  const thread_sched_stat_t oldstat =
    (thread_sched_stat_t)(oldvalue & T_STAT_MASK);
  const unsigned int ref = oldvalue & T_REF;
  int out;

  if((T_STAT_DEAD != oldstat && T_STAT_TERM != oldstat &&
      T_STAT_DESTROY != oldstat) || T_STAT_DESTROY == stat) {

    const unsigned int newvalue = ref | stat;

    out = atomic_compare_and_set_uint(oldvalue, newvalue,
				      &(thread->t_sched_stat_ref));

  }

  else
    out = -1;

  return out;

}


internal bool scheduler_update_thread(thread_t* const thread,
				      const thread_sched_stat_t stat) {

  INVARIANT(thread != NULL);
  INVARIANT(stat == T_STAT_SUSPEND || stat == T_STAT_TERM ||
	    stat == T_STAT_DESTROY || stat == T_STAT_GC_WAIT);

  unused int res;

  for(unsigned int i = 0;
      !(res = scheduler_try_update_thread(thread, stat));
      i++)
    backoff_delay(i);

  return 0 < res;

}


internal unsigned int sched_active_thread_count(void) {

  return sched_active_threads.value;

}
