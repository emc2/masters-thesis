/* Copyright (c) Eric McCorkle 2007, 2008.  All rights reserved. */

#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>

#include "definitions.h"
#include "atomic.h"
#include "os_thread.h"
#include "os_signal.h"
#include "cc.h"
#include "mm/mm_malloc.h"
#include "mm/gc_thread.h"
#include "mm/gc_vars.h"
#include "cc/executor.h"
#include "cc/scheduler.h"
#include "cc/context.h"

typedef struct executor_t {

  /*!
   * This is the id of the executor.  This will be used quite often
   * for many functions.  These are unique, beginning at 0 and
   * ascending, so they can be used as array indexes.
   *
   * \brief The executor's id.
   */
  unsigned int ex_id;

  /*!
   * This is the stack on which scheduler, GC, and malloc calls and
   * signals' initial handlers are processed.  This points to the
   * lowest address of the stack.  The "top" of the stack must be
   * calculated from this.
   *
   * \brief The stack for runtime operations.
   */
  volatile void* ex_c_stack;

  /*!
   * This is the highest available index for the GC write log.  If
   * this is equal to GC_WRITE_LOG_LENGTH, then the write log is full.
   *
   * \brief The highest available write log index.
   */
  unsigned int ex_gc_write_log_index;

  /*!
   * This is the OS thread ID used to manipulate the thread with
   * system calls.
   *
   * \brief The OS thread identifier structure.
   */
  os_thread_t ex_thread;

  /*!
   * This is the idle thread for this executor.  This is executed when
   * there are no other threads available.  This is equivalent to the
   * omega thread in the underlying model.
   *
   * \brief The idle thread.
   */
  thread_t ex_idle_thread;

  /*!
   * This is the garbage collector thread for this executor.  This is
   * executed when garbage collection is being done.
   *
   * \brief The garbage collector thread.
   */
  thread_t ex_gc_thread;


  /*!
   * This is the scheduler strucutre used by this executor.  This is
   * used to select the next thread.
   *
   * \brief The scheduler structure for this executor.
   */
  scheduler_t ex_scheduler;

  /*!
   * This is the garbage collector thread closure for this executor.
   * This contains thread-local data for the specific garbage
   * collector thread.
   *
   * \brief The GC closure for this executor.
   */
  gc_closure_t ex_gc_closure;

  /*!
   * This is the executor's signal mailbox.  This is used to
   * communicate signals to the threads running on this executor.
   *
   * \brief The signal mailbox for this executor.
   */
  volatile atomic_uint_t ex_signal_mbox;

  /*!
   * This is the garbage collection write log.  Writes record an entry
   * here, and when the log fills up, the garbage collector thread is
   * run.
   *
   * \brief The GC write log.
   */
  volatile gc_log_entry_t ex_gc_write_log[GC_WRITE_LOG_LENGTH];

  /*!
   * These are the garbage collection allocators for this executor.
   * The first allotator is for a store which will never be collected,
   * and is intended to hold objects that never go out of scope.  The
   * remaining correspond to each generation.
   *
   * \brief The allocators for this executor.
   */
  volatile gc_allocator_t ex_gc_allocators[];

} executor_t;


static unsigned int executor_num;
static executor_t* restrict executors;
static os_thread_key_t executor_key;
static os_thread_t executor_signal_thread;
static os_sigset_t executor_normal_sigmask;
static os_sigset_t executor_idle_sigmask;
static os_sigset_t executor_sigthread_sigmask;
static volatile atomic_uint_t executor_live;


internal unsigned int executor_request(cc_stat_t* restrict stat) {

  PRINTD("  Reserving space for executor system\n");

  const unsigned int os_thread_size = os_thread_request(stat);
  const unsigned int execs = stat->cc_num_executors;
  const unsigned int scheduler_size = scheduler_request(stat);
  const unsigned int executor_size = execs *
    (sizeof(executor_t) + (gc_num_generations * sizeof(gc_allocator_t)));
  const unsigned int aligned_executor_size =
    ((executor_size - 1) & ~(CACHE_LINE_SIZE - 1)) + CACHE_LINE_SIZE;

  PRINTD("    Reserving 0x%x bytes for executors\n", executor_size);
  PRINTD("    Reserving 0x%x bytes for scheduler system\n", scheduler_size);
  PRINTD("    Reserving 0x%x bytes for os thread system\n", os_thread_size);
  PRINTD("  Executor system total static size is 0x%x bytes.\n",
	 aligned_executor_size + scheduler_size + os_thread_size);

  return aligned_executor_size + scheduler_size + os_thread_size;

}


static noreturn void do_executor_sched_cycle(executor_t* exec);


static inline void executor_check_mbox_sigs(executor_t* const restrict exec,
					    const unsigned int sigs) {

  INVARIANT(exec != NULL);

  unsigned int mbox;

  PRINTD("Executor %u checking mailbox\n", exec->ex_id);

  /* get the mailbox value */
  for(unsigned int i = 0;; i++) {

    mbox = exec->ex_signal_mbox.value;

    if(atomic_compare_and_set_uint(mbox, 0, &(exec->ex_signal_mbox)))
      break;

    else
      backoff_delay(i);

  }

  mbox |= sigs;

  /* process each mailbox signal */
  if(mbox & EX_SIGNAL_SCHEDULE) {

    PRINTD("Executor %u is no longer idle\n", exec->ex_id);
    do_executor_sched_cycle(exec);

  }

  if(mbox & EX_SIGNAL_GC) {

    PRINTD("Executor %u is no longer idle\n", exec->ex_id);
    do_executor_run_gc(exec);

  }

}


static noreturn void executor_idle_thread(void) {

  executor_t* const restrict exec = os_thread_key_get(executor_key);

  /* FOR TESTING ONLY */
  INVARIANT(executors[!(exec->ex_id)].ex_scheduler.sch_curr_thread !=
	    executors[!(exec->ex_id)].ex_scheduler.sch_idle_thread);
  INVARIANT(exec != NULL);

  PRINTD("Executor %u in idle thread\n", exec->ex_id);
  /* Check the mailbox, and suspend with the idle mask if nothing
   * needs to happen
   */
  while(executor_live.value) {

    PRINTD("Executor %u checking mailbox\n", exec->ex_id);
    executor_check_mbox_sigs(exec, 0);
    PRINTD("Executor %u suspending\n", exec->ex_id);
    os_sigsuspend(&executor_idle_sigmask);

  }

  PRINTD("Executor %u received termination while idle\n",
	  exec->ex_id);
  os_thread_exit(NULL);

}


static noreturn void executor_gc_thread(void) {

  executor_t* const restrict exec = os_thread_key_get(executor_key);

  gc_thread(&(exec->ex_gc_closure), exec->ex_id);
  PRINTD("Executor %u garbage collection function returned\n",
	  exec->ex_id);
  os_thread_exit(NULL);

}


static void* executor_signal_thread_start(unused void* const arg) {

  while(executor_live.value)
    os_sigsuspend(&executor_sigthread_sigmask);

  return NULL;

}


static inline void executor_setup_threads(executor_t* const restrict exec,
					  volatile void* const stkptr) {

  INVARIANT(exec != NULL);
  INVARIANT(stkptr != NULL);

  volatile void* const aligned_stkptr =
    context_align_stkptr(stkptr, STACK_ALIGN);
  volatile retaddr_t * const idle_retaddr_ptr =
    thread_mbox_retaddr(exec->ex_idle_thread.t_mbox);
  volatile unsigned int* const idle_executor_ptr =
    thread_mbox_executor(exec->ex_idle_thread.t_mbox);
  volatile void* volatile * const idle_stkptr_ptr =
    thread_mbox_stkptr(exec->ex_idle_thread.t_mbox);
  volatile atomic_uint_t* volatile * const idle_sigptr_ptr =
    thread_mbox_sigptr(exec->ex_idle_thread.t_mbox);
  volatile unsigned int* const idle_write_log_index_ptr =
    thread_mbox_write_log_index(exec->ex_idle_thread.t_mbox);
  volatile gc_log_entry_t* volatile* const idle_write_log_ptr =
    thread_mbox_write_log(exec->ex_idle_thread.t_mbox);
  volatile gc_allocator_t* volatile* const idle_allocator_ptr =
    thread_mbox_allocators(exec->ex_idle_thread.t_mbox);
  volatile retaddr_t * const gc_retaddr_ptr =
    thread_mbox_retaddr(exec->ex_gc_thread.t_mbox);
  volatile unsigned int* const gc_executor_ptr =
    thread_mbox_executor(exec->ex_gc_thread.t_mbox);
  volatile void* volatile * const gc_stkptr_ptr =
    thread_mbox_stkptr(exec->ex_gc_thread.t_mbox);
  volatile atomic_uint_t* volatile * const gc_sigptr_ptr =
    thread_mbox_sigptr(exec->ex_gc_thread.t_mbox);
  volatile unsigned int* const gc_write_log_index_ptr =
    thread_mbox_write_log_index(exec->ex_gc_thread.t_mbox);
  volatile gc_log_entry_t* volatile* const gc_write_log_ptr =
    thread_mbox_write_log(exec->ex_gc_thread.t_mbox);
  volatile gc_allocator_t* volatile* const gc_allocator_ptr =
    thread_mbox_allocators(exec->ex_gc_thread.t_mbox);

  PRINTD("Creating idle thread %p for executor %u\n",
	 &(exec->ex_idle_thread), exec->ex_id);
  exec->ex_idle_thread.t_id = 0;
  exec->ex_idle_thread.t_sched_stat_ref.value = T_STAT_RUNNING | T_REF;
  exec->ex_idle_thread.t_destroy = NULL;
  exec->ex_idle_thread.t_rlist_next = NULL;
  exec->ex_idle_thread.t_queue_next = NULL;
  *idle_retaddr_ptr = executor_idle_thread;
  *idle_stkptr_ptr = aligned_stkptr;
  *idle_executor_ptr = thread_mbox_null_executor;
  *idle_sigptr_ptr = &(exec->ex_signal_mbox);
  *idle_write_log_index_ptr = 0;
  *idle_write_log_ptr = exec->ex_gc_write_log;
  *idle_allocator_ptr = exec->ex_gc_allocators;
  PRINTD("Creating gc thread %p for executor %u\n",
	 &(exec->ex_gc_thread), exec->ex_id);
  exec->ex_gc_thread.t_id = 0;
  exec->ex_gc_thread.t_sched_stat_ref.value = T_STAT_RUNNING | T_REF;
  exec->ex_gc_thread.t_destroy = NULL;
  exec->ex_gc_thread.t_rlist_next = NULL;
  exec->ex_gc_thread.t_queue_next = NULL;
  *gc_retaddr_ptr = executor_gc_thread;
  *gc_stkptr_ptr = aligned_stkptr;
  *gc_executor_ptr = thread_mbox_null_executor;
  *gc_sigptr_ptr = &(exec->ex_signal_mbox);
  *gc_write_log_index_ptr = 0;
  *gc_write_log_ptr = exec->ex_gc_write_log;
  *gc_allocator_ptr = exec->ex_gc_allocators;

}


/* This sets up the idle thread, the C stack, and the signals */
static inline void executor_init_state(executor_t* const restrict exec,
				       volatile void* const stkptr) {

  INVARIANT(exec != NULL);

  PRINTD("Executor %u initializing state\n", exec->ex_id);
  exec->ex_c_stack = stkptr;
  PRINTD("Executor %u clearing write log\n", exec->ex_id);
  memset(exec->ex_gc_write_log, 0,
	 GC_WRITE_LOG_LENGTH * sizeof(gc_log_entry_t));
  PRINTD("Executor %u initializing gc closure\n", exec->ex_id);
  gc_closure_init(&(exec->ex_gc_closure), exec->ex_gc_write_log);
  executor_setup_threads(exec, stkptr);
  scheduler_init(&(exec->ex_scheduler), &(exec->ex_idle_thread),
		 &(exec->ex_gc_thread));
  PRINTD("Executor %u setting signal state\n", exec->ex_id);
  os_thread_sigmask_set(&executor_normal_sigmask, NULL);
  os_thread_key_set(executor_key, exec);
  PRINTD("Executor %u finished initializing state\n", exec->ex_id);

}


/* This function starts an executor other than the initial one.  It
 * creates the idle thread, sets up the signal stuff, runs the
 * scheduler, and starts execution.
 */
static inline noreturn void executor_worker_thread(executor_t* const
						   restrict exec) {

  INVARIANT(exec != NULL);

  volatile void* const stkptr = context_curr_stkptr();

  PRINTD("Executor %u coming online\n", exec->ex_id);
  executor_init_state(exec, stkptr);
  do_executor_sched_cycle(exec);

}


static void* executor_worker_start(void* const restrict arg) {

  executor_worker_thread(arg);

  return NULL;

}


static void acknowledge(unused int sig) {}


internal unsigned int executor_count(void) {

  return executor_num;

}


static inline void* executor_setup(const unsigned int num,
				   unused const unsigned int stack_size,
				   void* const mem) {

  INVARIANT(num > 1);

  const unsigned int executor_size = num * sizeof(executor_t);
  const unsigned int aligned_executor_size =
    0 != executor_size ? ((executor_size - 1) & ~(CACHE_LINE_SIZE - 1))
    + CACHE_LINE_SIZE : 0;
  void* ptr = mem;
  os_sigset_t full;
  os_sigset_t old;

  PRINTD("Initializing executor data structures for %u executors\n", num);
  /* Initialize the signal masks */
  PRINTD("Preparing signal state\n");
  os_sigset_fill(&executor_idle_sigmask);
  os_thread_sigset_clear_mandatory(&executor_idle_sigmask);
  os_sigset_clear(&executor_idle_sigmask, os_signal_check_mbox);
  os_sigset_fill(&executor_normal_sigmask);
  os_thread_sigset_clear_mandatory(&executor_normal_sigmask);
  os_sigset_empty(&executor_sigthread_sigmask);
  os_sigset_fill(&full);

  /* Block all signals for this process */
  PRINTD("Blocking signals to process\n");
  os_proc_sigmask_block(&full, &old);
  os_sig_handler(acknowledge, os_signal_check_mbox);

  /* Set the current thread's signal mask */
  PRINTD("Setting current thread's signal mask\n");
  os_thread_sigmask_set(&executor_normal_sigmask, NULL);

  /* Create the key for the current executor */
  PRINTD("Creating current executor key\n");
  os_thread_key_create(&executor_key);

  /* Create the signal thread */
  PRINTD("Starting signal thread\n");
  executor_signal_thread = os_thread_create(executor_signal_thread_start, NULL);

  /* Create the executor structures */
  PRINTD("Creating executor structures\n");
  executor_live.value = 1;
  executor_num = num;
  PRINTD("Executors array is in static memory at 0x%p\n", mem);
  executors = ptr;
  ptr = (char*)ptr + aligned_executor_size;

  /* The zero entry is the master thread, which shuts the system down
   * at the end.
   */
  PRINTD("Initializing self\n");
  executors[0].ex_id = 0;
  ptr = scheduler_setup(&(executors[0].ex_scheduler), ptr);
  executors[0].ex_signal_mbox.value = 0;
  executors[0].ex_thread = os_thread_self();

  /* Initialize the other executors */
  for(unsigned int i = 1; i < num; i++) {

    PRINTD("Initializing executor %u\n", i);
    executors[i].ex_id = i;
    ptr = scheduler_setup(&(executors[i].ex_scheduler), ptr);
    executors[i].ex_signal_mbox.value = 0;
    PRINTD("Starting OS thread\n");
    executors[i].ex_thread =
      os_thread_create(executor_worker_start, executors + i);

  }


  /* Allow all signals for the process */
  PRINTD("Unblocking signals\n");
  os_proc_sigmask_set(&old, NULL);
  PRINTD("Executor initialization complete\n");

  INVARIANT(executors != NULL);
  INVARIANT(executor_live.value != 0);

  return ptr;

}


/* This is called by the original thread, after creating all other
 * executors, to start the program
 */
static inline noreturn
void executor_start_prog(void (*const main)(thread_t* restrict thread,
					    unsigned int exec,
					    unsigned int argc,
					    const char* const * argv,
					    const char* const * envp),
			 volatile void* const stkptr,
			 const unsigned int argc,
			 const char* const * const argv,
			 const char* const * const envp,
			 void* const mem) {

  INVARIANT(stkptr != NULL);
  INVARIANT(main != NULL);
  INVARIANT(argv != NULL);
  INVARIANT(envp != NULL);

  thread_t* start_thread;
  thread_stat_t stat;
  thread_mbox_t mbox;
  volatile void* const aligned_stkptr =
    context_align_stkptr(stkptr, STACK_ALIGN);
  volatile retaddr_t * const retaddr_ptr = thread_mbox_retaddr(mbox);
  volatile unsigned int* const executor_ptr = thread_mbox_executor(mbox);
  volatile void* volatile * const stkptr_ptr = thread_mbox_stkptr(mbox);
  volatile atomic_uint_t* volatile * const sigptr_ptr =
    thread_mbox_sigptr(mbox);
  volatile unsigned int* const write_log_index_ptr =
    thread_mbox_write_log_index(mbox);
  volatile gc_log_entry_t* volatile* const write_log_ptr =
    thread_mbox_write_log(mbox);
  volatile gc_log_entry_t* volatile* const allocator_ptr =
    thread_mbox_allocators(mbox);

  unused void* ptr = mem;

#ifdef INTERACTIVE
#error "Set the main thread's priority"
#endif
  executor_init_state(executors + 0, stkptr);
  PRINTD("Initializing program start thread\n");

  /* Create the initial thread */
  start_thread = malloc_lf(sizeof(thread_t), 0);
  *retaddr_ptr = 0;
  *stkptr_ptr = aligned_stkptr;
  *executor_ptr = 0;
  *sigptr_ptr = NULL;
  *write_log_index_ptr = 0;
  *write_log_ptr = executors[0].ex_gc_write_log;
  *allocator_ptr = executors[0].ex_gc_allocators;
  stat.t_sched_stat = T_STAT_RUNNABLE;
  stat.t_destroy = (void (*)(thread_t* ptr))free;
  thread_init(start_thread, &stat, mbox);
  start_thread->t_sched_stat_ref.value = T_STAT_RUNNING | T_REF;
  PRINTD("Setting current thread to start thread\n");
  executors[0].ex_scheduler.sch_curr_thread = start_thread;
  executors[0].ex_scheduler.sch_num_threads = 1;
  PRINTD("Starting program\n");
  main(start_thread, 0, argc, argv, envp);

  /* If main returns, kill the program */
  executor_stop(0);

}


/* This is called by whatever executor initiates a shutdown, to
 * actually shut the system down.
 */
static inline noreturn void executor_shutdown(const unsigned int exec) {

  INVARIANT(executor_live.value == 0);

  PRINTD("Executor %u executing shutdown sequence\n", exec);
  PRINTD("Executor %u sending signal thread check mailbox signal\n", exec);
  os_thread_signal_send(executor_signal_thread, os_signal_check_mbox);
  os_thread_join(executor_signal_thread);

  for(unsigned int i = 0; i < executor_num; i++)
    if(i != exec) {

      PRINTD("Executor %u sending executor %u check mailbox signal\n", exec, i);
      os_thread_signal_send(executors[i].ex_thread, os_signal_check_mbox);
      PRINTD("Executor %u joining executor %u\n", exec, i);
      os_thread_join(executors[i].ex_thread);
      PRINTD("Executor %u destroying scheduler %u\n", exec, i);
      scheduler_destroy(&(executors[i].ex_scheduler));

    }

  PRINTD("Executor %u destroying own structures\n", exec);
  scheduler_destroy(&(executors[exec].ex_scheduler));
  scheduler_stop(exec);
  os_thread_key_destroy(executor_key);
  PRINTD("Executor %u exiting\n", exec);
  os_thread_exit(NULL);

}


internal void executor_start(const unsigned int num,
			     const unsigned int stack_size,
			     void (*const main) (thread_t* restrict thread,
						 unsigned int exec,
						 unsigned int argc,
						 const char* const * argv,
						 const char* const * envp),
			     const unsigned int argc,
			     const char* const * const argv,
			     const char* const * const envp,
			     void* const mem) {

  INVARIANT(num > 1);
  INVARIANT(stack_size != 0);
  INVARIANT(!(stack_size % PAGE_SIZE));
  INVARIANT(main != NULL);
  INVARIANT(argv != NULL);
  INVARIANT(envp != NULL);

  /* Take the stack pointer here.  The caller may have allocated some
   * things on the stack and I don't want to wipe them out.
   */
  volatile void* const stkptr = context_curr_stkptr();
  void* const ptr = scheduler_start(num, mem);
  void* const end = executor_setup(num, stack_size, ptr);

  PRINTD("Executor memory:\n");
  PRINTD("\tscheduler system at 0x%p\n", mem);
  PRINTD("\texecutors at 0x%p\n", ptr);
  PRINTD("\tend at 0x%p\n", end);
  executor_start_prog(main, stkptr, argc, argv, envp, end);

}


internal noreturn void executor_stop(const unsigned int exec) {

  PRINTD("Executor %u stopping system\n", exec);
  executor_live.value = 0;
  executor_shutdown(exec);

}


internal void executor_safepoint(const unsigned int exec,
				 const unsigned int sigs) {

  executor_check_mbox_sigs(executors + exec, sigs);

}


static inline void executor_retire_old_thread(executor_t* const restrict exec) {

  thread_t* const thread = exec->ex_scheduler.sch_curr_thread;

  if(NULL != thread) {

    PRINTD("Executor %u storing null executor code to context %p\n",
	   exec->ex_id, thread->t_mbox);
    volatile unsigned int* const executor_ptr =
      thread_mbox_executor(thread->t_mbox);
    volatile unsigned int* const write_log_index_ptr =
      thread_mbox_write_log_index(thread->t_mbox);

    exec->ex_gc_write_log_index = *write_log_index_ptr;
    *executor_ptr = thread_mbox_null_executor;

  }

}


static inline noreturn void executor_get_new_thread(executor_t* const
						    restrict exec) {

  thread_t* const thread = scheduler_cycle(&(exec->ex_scheduler), exec->ex_id);
  volatile retaddr_t* const retaddr_ptr =
    thread_mbox_retaddr(thread->t_mbox);
  volatile unsigned int* const executor_ptr =
    thread_mbox_executor(thread->t_mbox);
  volatile void* volatile * const stkptr_ptr =
    thread_mbox_stkptr(thread->t_mbox);
  volatile atomic_uint_t* volatile * const sigptr_ptr =
    thread_mbox_sigptr(thread->t_mbox);
  volatile unsigned int* const write_log_index_ptr =
    thread_mbox_write_log_index(thread->t_mbox);
  volatile gc_log_entry_t* volatile* const write_log_ptr =
    thread_mbox_write_log(thread->t_mbox);
  volatile gc_log_entry_t* volatile* const allocator_ptr =
    thread_mbox_allocators(thread->t_mbox);
  volatile void* const stkptr = *stkptr_ptr;
  const retaddr_t retaddr = *retaddr_ptr;

  PRINTD("Executor %u storing return context: (%u, %p)\n",
	 exec->ex_id, exec->ex_id, exec->ex_c_stack);
  *executor_ptr = exec->ex_id;
  *stkptr_ptr = exec->ex_c_stack;
  *sigptr_ptr = &(exec->ex_signal_mbox);
  *write_log_index_ptr = exec->ex_gc_write_log_index;
  *write_log_ptr = exec->ex_gc_write_log;
  *allocator_ptr = exec->ex_gc_allocators;
  store_fence();
  /* This cannot change as long as I'm here */
  PRINTD("Executor %u loading context: %p (%p, %p)\n",
	 exec->ex_id, thread->t_mbox, retaddr, stkptr);
  context_load(retaddr, stkptr);

}


static noreturn void do_executor_sched_cycle(executor_t* const exec) {

  PRINTD("Executor %u cycling scheduler\n", exec->ex_id);

  if(executor_live.value) {

    executor_retire_old_thread(exec);
    executor_get_new_thread(exec);

  }

  else {

    PRINTD("Executor %u acknowledged termination in scheduler\n",
	   exec);
    os_thread_exit(NULL);

  }

}


static noreturn void do_executor_run_gc(executor_t* const exec) {

  PRINTD("Executor %u cycling scheduler\n", exec->ex_id);

  if(executor_live.value) {

    executor_retire_old_thread(exec);
    executor_get_new_thread(exec);

  }

  else {

    PRINTD("Executor %u acknowledged termination in scheduler\n",
	   exec);
    os_thread_exit(NULL);

  }

}


internal noreturn void executor_sched_cycle(const unsigned int exec) {

  do_executor_sched_cycle(executors + exec);

}


static inline bool executor_try_signal(executor_t* const restrict exec,
				       const unsigned int sigs) {

  const unsigned int oldsigs = exec->ex_signal_mbox.value;

  return atomic_compare_and_set_uint(oldsigs, oldsigs | sigs,
				     &(exec->ex_signal_mbox));


}


internal void executor_raise(const unsigned int exec,
			     const unsigned int sigs) {

  executor_t* const ex = executors + exec;

  PRINTD("Sending executor %u signals %x\n", exec, sigs);

  for(unsigned int i = 0; !executor_try_signal(ex, sigs); i++)
    backoff_delay(i);

  if(ex->ex_scheduler.sch_curr_thread == ex->ex_scheduler.sch_idle_thread) {

    PRINTD("Sending executor %u mailbox check signal\n", exec);
    os_thread_signal_send(executors[exec].ex_thread, os_signal_check_mbox);

  }

}


internal unsigned int executor_self(void) {

  const executor_t* const restrict exec = os_thread_key_get(executor_key);

  return exec->ex_id;

}


static inline int executor_try_wakeup(executor_t* const restrict exec) {

  INVARIANT(exec != NULL);

  int out;
  const unsigned int oldsigs = exec->ex_signal_mbox.value;

  if(NULL == exec->ex_scheduler.sch_curr_thread &&
     !(oldsigs & EX_SIGNAL_SCHEDULE)) {

    PRINTD("Attempting to wake suspended executor %u\n", exec->ex_id);
    if(out = atomic_compare_and_set_uint(oldsigs, oldsigs | EX_SIGNAL_SCHEDULE,
					 &(exec->ex_signal_mbox)))
      os_thread_signal_send(exec->ex_thread, os_signal_check_mbox);

  }

  else {

    PRINTD("Executor %u is awake or waiting for a signal\n", exec->ex_id);
    out = -1;

  }

  return out;

}


internal void executor_restart_idle(void) {

  INVARIANT(executors != NULL);
  PRINTD("Attempting to wake suspended executors\n");

  for(unsigned int i = 0; i < executor_num; i++) {

    int res;

    PRINTD("Checking executor %u for wakeup\n", i);

    for(unsigned int j = 0; !(res = executor_try_wakeup(executors + i)); j++)
      backoff_delay(j);

    if(1 == res)
      break;

  }

}
