/* Copyright (c) 2007, 2008 Eric McCorkle.  All rights reserved. */

#include <string.h>

#include "definitions.h"
#include "atomic.h"
#include "cc/thread.h"

/* Global invariant: bijection between increments and thread
 * creations, and decrements and thread destructions.
 */
static volatile atomic_uint_t thread_num;

/* Global invariant: monotonically increasing */
static volatile atomic_uint_t thread_id;

internal void thread_init(thread_t* restrict thread,
			  const thread_stat_t* restrict stat,
			  const thread_mbox_t mbox) {

  INVARIANT(thread != NULL);
  INVARIANT(stat->t_sched_stat == T_STAT_RUNNABLE ||
	    stat->t_sched_stat == T_STAT_SUSPEND);

  PRINTD("Initializing thread %p\n", thread);
  thread->t_id = atomic_fetch_inc_uint(&thread_id);
  atomic_increment_uint(&thread_num);
#ifdef INTERACTIVE
  thread->t_hard_pri = stat->t_pri;
#endif
  /* A thread cannot be initialized to RUNNABLE.  This must be set by
   * the activate function.  Set to NONE instead.
   */
  thread->t_sched_stat_ref.value =
    T_STAT_RUNNABLE == stat->t_sched_stat ? T_STAT_NONE : stat->t_sched_stat;
  memcpy((void*)(thread->t_mbox), mbox, sizeof(thread_mbox_t));
  PRINTD("State/ref count: %x\n", thread->t_sched_stat_ref.value);
  PRINTD("Initial mailbox state for thread %p: %p = [%p, %p, %p, %p\n"
	 "                                           %p, %p, %p]\n",
	 thread, thread->t_mbox,
	 ((void**)(thread->t_mbox))[0], ((void**)(thread->t_mbox))[1],
	 ((void**)(thread->t_mbox))[2], ((void**)(thread->t_mbox))[3],
	 ((void**)(thread->t_mbox))[4], ((void**)(thread->t_mbox))[5],
	 ((void**)(thread->t_mbox))[6]);

  INVARIANT((thread->t_sched_stat_ref.value & T_STAT_MASK) == 
	    stat->t_sched_stat ||
	    ((thread->t_sched_stat_ref.value & T_STAT_MASK) == T_STAT_NONE &&
	     stat->t_sched_stat == T_STAT_RUNNABLE));
  INVARIANT(!(thread->t_sched_stat_ref.value & T_REF));
  /* Global invariants:
   * - Thread ID's are unique.
   * - thread_id is monotonically increasing.
   * - Bijection between thread creations and increments to thread_num
   */

}


internal void thread_destroy(unused thread_t* restrict thread) {

  INVARIANT(thread != NULL);

  PRINTD("Destroying thread %p\n", thread);
  atomic_decrement_uint(&thread_num);

  if(NULL != thread->t_destroy)
    thread->t_destroy(thread);

  /* Global invariants:
   * - Bijection between thread deletions and decrements to thread_num
   */

}


internal unsigned int thread_count(void) {

  return thread_num.value;

}


internal pure volatile retaddr_t*
thread_mbox_retaddr(volatile thread_mbox_t mbox) {

  volatile retaddr_t* const out = (volatile retaddr_t*)mbox;

  return out;

}


internal pure volatile void* volatile *
thread_mbox_stkptr(volatile thread_mbox_t mbox) {

  volatile retaddr_t* const ptr = thread_mbox_retaddr(mbox);
  volatile void* volatile * const out = (volatile void* volatile *)(ptr + 1);

  return out;

}


internal pure volatile unsigned int*
thread_mbox_executor(volatile thread_mbox_t mbox) {

  volatile void* volatile * const ptr = thread_mbox_stkptr(mbox);
  volatile unsigned int* const out = (volatile unsigned int*)(ptr + 1);

  return out;

}


internal pure volatile atomic_uint_t* volatile *
thread_mbox_sigptr(volatile thread_mbox_t mbox) {

  volatile unsigned int* const ptr = thread_mbox_executor(mbox);
  volatile atomic_uint_t* volatile * const out =
    (volatile atomic_uint_t* volatile *)(ptr + 1);

  return out;

}


internal pure volatile unsigned int*
thread_mbox_write_log_index(volatile thread_mbox_t mbox) {

  volatile atomic_uint_t* volatile * const ptr = thread_mbox_sigptr(mbox);
  volatile unsigned int* const out = (volatile unsigned int*)(ptr + 1);

  return out;

}


internal pure volatile gc_log_entry_t* volatile *
thread_mbox_write_log(volatile thread_mbox_t mbox) {

  volatile unsigned int* const ptr = thread_mbox_write_log_index(mbox);
  volatile gc_log_entry_t* volatile * const out =
    (volatile gc_log_entry_t* volatile *)(ptr + 1);

  return out;

}


internal pure volatile gc_allocator_t* volatile *
thread_mbox_allocators(volatile thread_mbox_t mbox) {

  volatile gc_log_entry_t* volatile *const ptr = thread_mbox_write_log(mbox);
  volatile gc_allocator_t* volatile * const out =
    (volatile gc_allocator_t* volatile *)(ptr + 1);

  return out;

}
