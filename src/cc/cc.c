/* Copyright (c) 2007, 2008 Eric McCorkle.  All rights reserved. */
#include <assert.h>

#include "definitions.h"
#include "cc.h"
#include "panic.h"

#include "../arch/context.c"
#include "os/os_signal.c"
#include "lf_thread_queue.c"
#include "thread.c"

#ifdef INTERACTIVE
#define CC_VARIANT "Interactive"
#error "Interactive scheduler not implemented"
#else
#define CC_VARIANT "Non-Interactive"
#include "noninteract_scheduler.c"
#endif

#ifdef ACTIVATION
#define CC_EXECUTOR "Activation-Based"
#include "os/os_activation.c"
#else
#define CC_EXECUTOR "Thread-Based"
#include "os/os_thread.c"
#include "thread_executor.c"
#endif


const char* cc_name = "Parallel, " CC_VARIANT ", " CC_EXECUTOR;


unsigned int cc_request(cc_stat_t* const restrict stat) {

  PRINTD("Reserving space for concurrency system\n");

  const unsigned int executor_size = executor_request(stat);

  PRINTD("  Reserving 0x%x bytes for executor system\n", executor_size);
  PRINTD("Concurrency system total static size is 0x%x bytes.\n",
	 executor_size);

  return executor_size;

}


noreturn void cc_start(cc_stat_t* const restrict stat,
		       void (* const main)(thread_t* restrict thread,
					   unsigned int exec,
					   unsigned int argc,
					   const char* const * argv,
					   const char* const * envp),
		       const unsigned int argc,
		       const char* const * const argv,
		       const char* const * const envp,
		       void* const mem) {

  INVARIANT(stat != NULL);
  INVARIANT(main != NULL);
  INVARIANT(argv != NULL);
  INVARIANT(envp != NULL);

  stat->cc_num_executors = stat->cc_num_executors > 1 ?
    stat->cc_num_executors : 2;
  PRINTD("Initializing concurrency system with %u executors\n",
	 stat->cc_num_executors);
  executor_start(stat->cc_num_executors,
		 stat->cc_executor_stack_size,
		 main, argc, argv, envp, mem);

}


noreturn void cc_stop(const unsigned int exec) {

  INVARIANT(exec == executor_self());

  PRINTD("Executor %u stopping concurrency system.\n", exec);
  executor_stop(exec);

}


void cc_stat(cc_stat_t* const restrict stat) {

  stat->cc_num_executors = executor_count();
  stat->cc_actual_executors = stat->cc_num_executors;
  stat->cc_executor_stack_size = 0;
  stat->cc_max_threads = 0;
  stat->cc_num_threads = thread_count();
  stat->cc_active_threads = sched_active_thread_count();

}


noreturn void cc_sched_cycle(const unsigned int exec) {

  INVARIANT(exec == executor_self());

  executor_sched_cycle(exec);

}


extern void cc_thread_create(const thread_stat_t* const restrict stat,
			     const thread_mbox_t mbox,
			     volatile void* volatile * const mbox_addr_ptr,
			     thread_t* const restrict thread,
			     const unsigned int exec) {

  PRINTD("Executor %u creating a thread.\n", exec);
  INVARIANT(mbox_addr_ptr != NULL);
  INVARIANT(stat != NULL);
  INVARIANT(thread != NULL);
  INVARIANT(exec == executor_self());

  PRINTD("New thread is at %p.\n", thread);

  if(NULL != thread) {

    PRINTD("Mailbox is at %p, storing addess to %p\n",
	   &(thread->t_mbox), mbox_addr_ptr);
    *mbox_addr_ptr = &(thread->t_mbox);
    PRINTD("Mailbox pointer pointer %p holds [%p] = (%p, %p, %p, %p) "
	   "before initialization\n",
	   mbox_addr_ptr, *mbox_addr_ptr, (*(void***)mbox_addr_ptr)[0],
	   (*(void***)mbox_addr_ptr)[1], (*(void***)mbox_addr_ptr)[2],
	   (*(void***)mbox_addr_ptr)[3]);
    INVARIANT(*mbox_addr_ptr == &(thread->t_mbox));
    thread_init(thread, stat, mbox);
    PRINTD("Mailbox pointer pointer %p holds [%p] = (%p, %p, %p, %p) "
	   "after initialization\n",
	   mbox_addr_ptr, *mbox_addr_ptr, (*(void***)mbox_addr_ptr)[0],
	   (*(void***)mbox_addr_ptr)[1], (*(void***)mbox_addr_ptr)[2],
	   (*(void***)mbox_addr_ptr)[3]);
    INVARIANT(*mbox_addr_ptr == &(thread->t_mbox));
    /* Remember that the thread's status will not be RUNNABLE, if the
     * initial status is.  RUNNABLE can only be set by the activate
     * function.
     */
    store_fence();
    if(T_STAT_RUNNABLE == stat->t_sched_stat)
      scheduler_activate_thread(thread, exec);
    INVARIANT(*mbox_addr_ptr == &(thread->t_mbox));
  }

  else
    panic("Error in runtime: Cannot allocate memory");

  PRINTD("Executor %u created thread %p.\n", exec, thread);
  INVARIANT(*mbox_addr_ptr == &(thread->t_mbox));

}


void cc_thread_destroy(thread_t* const restrict thread,
		       unused const unsigned int exec) {

  PRINTD("Executor %u updating thread %p, status is %x.\n",
	 exec, thread, thread->t_sched_stat_ref.value);
  INVARIANT((thread->t_sched_stat_ref.value & T_STAT_MASK) !=
	    T_STAT_DESTROY);
  INVARIANT(exec == executor_self());
  INVARIANT(thread != NULL);
  PRINTD("Executor %u destroying thread %p.\n", exec, thread);

  unused bool out = scheduler_deactivate_thread(thread, T_STAT_DESTROY, exec);

}


void cc_thread_update(thread_t* const restrict thread,
		      const thread_stat_t* const restrict stat,
		      unused const unsigned int exec,
		      bool* const restrict result) {

  PRINTD("Executor %u updating thread %p, status is %x.\n",
	 exec, thread, thread->t_sched_stat_ref.value & T_STAT_MASK);
  INVARIANT((thread->t_sched_stat_ref.value & T_STAT_MASK)
	    != T_STAT_DESTROY);
  INVARIANT(thread != NULL);
  INVARIANT(stat != NULL);
  INVARIANT(exec == executor_self());

  bool out;

  if(T_STAT_NONE != stat->t_sched_stat) {

    PRINTD("Executor %u updating thread %p status to %x.\n",
	   exec, thread, stat->t_sched_stat);

    if(T_STAT_RUNNABLE == stat->t_sched_stat ||
       T_STAT_SUSPEND == stat->t_sched_stat ||
       T_STAT_TERM == stat->t_sched_stat ||
       T_STAT_GC_WAIT == stat->t_sched_stat)
      out = scheduler_update_thread(thread, stat->t_sched_stat);

    else {

      PRINTD("Invalid status.\n");
      out = false;

    }

  }

  else
    out = true;

  *result = out;

}


void cc_thread_update_immediate(thread_t* const restrict thread,
				const thread_stat_t* const restrict stat,
				const unsigned int exec,
				bool* const restrict result) {

  PRINTD("Executor %u updating thread %p immediately, status is %x.\n",
	 exec, thread, thread->t_sched_stat_ref.value);
  INVARIANT((thread->t_sched_stat_ref.value & T_STAT_MASK)
	    != T_STAT_DESTROY);
  INVARIANT(thread != NULL);
  INVARIANT(stat != NULL);
  INVARIANT(exec == executor_self());

  bool out;

  if(T_STAT_NONE != stat->t_sched_stat) {

    PRINTD("Executor %u updating thread %p status to %x immediately.\n",
	   exec, thread, stat->t_sched_stat);

    switch(stat->t_sched_stat) {

    case T_STAT_RUNNABLE:

      out = scheduler_activate_thread(thread, exec);
      break;

    case T_STAT_SUSPEND:

      out = scheduler_deactivate_thread(thread, T_STAT_SUSPEND, exec);
      break;

    case T_STAT_TERM:

      out = scheduler_deactivate_thread(thread, T_STAT_TERM, exec);
      break;

    case T_STAT_DESTROY:

      panic("Error in runtime: thread %p cannot be destroyed this way\n",
	    thread);

    default:

      out = false;
      PRINTD("Invalid status.");
      break;

    }

    PRINTD("Executor %u status update %s\n", exec,
	   1 == out ? "succeeded" : "failed");

  }

  else
    out = true;

  *result = out;

}


void cc_executor_id(unsigned int* const restrict id) {

  *id = executor_self();

}


void cc_safepoint(const unsigned int exec,
		  const unsigned int sigs) {

  INVARIANT(exec == executor_self());

  PRINTD("Executor %d is executing a safepoint with signal set %x\n",
	 exec, sigs);

  executor_safepoint(exec, sigs);

}
