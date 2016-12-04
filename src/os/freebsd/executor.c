/* Copyright (c) Eric McCorkle 2007.  All rights reserved. */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/kse.h>

#include "definitions.h"
#include "slice.h"
#include "executor.h"

static typedef struct executor_t {

  struct kse_mailbox ex_kse_mbox;
  struct executor_t* ex_next;
  unsigned int ex_id;

} executor_t;

/*!
 * This function sets the number of executors operating in the system.
 * In this implementation, this function is ineffectual.
 *
 * \brief Does nothing (number of executors cannot be changed).
 * \arg num The number of executors to operate.
 */
internal void executor_set_count(unused unsigned int num) {}


static void executor_scheduler(struct kse_mailbox* mbox) {

  static atomic_ptr_t suspended;
  executor_t* const executor = mbox->km_udata;
  thread_t* newthread;

  /* XXX wake up the returning threads too */

  if(NULL != newthread) {

    mbox->km_curthread = thread->th_kse_mbox;
    kse_switchin(thread->th_kse_mbox, 0);
    perror("Runtime error occurred in scheduler:\n");
    exit(EXIT_FAILURE);

  }

  else {

    for(unsigned int i = 0;; i++) {

      executor_t* oldlist = suspended.value;

      executor->ex_next = oldlist;

      if(atomic_compare_and_set(oldlist, executor, suspended))
	break;

      else
	backoff_delay_nosleep(i);

    }

    kse_release(NULL);
    panic("Runtime error occurred in scheduler:\n");

  }

}


/*!
 * This function creates the requested number of executors and starts
 * the thread schedulers.
 *
 * \brief Start the system, creating the requested number of executors
 * and their schedulers.
 * \arg num The requested number of executors.
 * \arg stack_size The size of the executor's stack (must be a
 * multiple of PAGE_SIZE).
 * \arg main The entry point into the guest program.
 */
internal void executor_start(unsigned int num, unsigned int stack_size,
			     void (*main)(void)) {

  INVARIANT(num > 1);
  INVARIANT(stack_size >= PAGE_SIZE);
  INVARIANT(0 == stack_size % PAGE_SIZE);
  INVARIANT(main != NULL);

  const unsigned int executor_space =
    0 != (sizeof(executor_t) % PAGE_SIZE) ?
    sizeof(executor_t) + (PAGE_SIZE - (sizeof(executor_t) % PAGE_SIZE)) : 
    sizeof(executor_t);
  const unsigned int one_executor = executor_space + stack_size;
  const unsigned int total_space = num * one_executor;
  const unsigned int slice_size = total_space < slice_min_size;
  slice_t* const executors_slice =
    slice_alloc(SLICE_TYPE_STATIC, SLICE_PROT_RWX, slice_size);
   
  if(NULL != executors_slice) {

    struct kse_thr_mailbox* thread_mbox;
    thread_t* main_thread;
    executor_t* executor = executors_slice;

    /* The first thread stays in this context for a bit. */
    executor->ex_id = 1;
    executor->ex_next = NULL;
    executor->ex_kse_mbox.km_version = KSE_VER_0;
    executor->ex_kse_mbox.km_curthread = NULL;
    executor->ex_kse_mbox.km_completed = NULL;
    sigemptyset(&(executor->ex_kse_mbox.km_sigscaught));
    executor->ex_kse_mbox.km_flags = 0;
    executor->ex_kse_mbox.km_func = executor_scheduler;
    executor->ex_kse_mbox.km_stack = (char*)executor + executor_space;
    executor->ex_kse_mbox.km_udata = executor;

    if(kse_create(&(executor->ex_kse_mbox), 0))
      panic("Failed to initialize executor");
 
    executor = (executor_t*)((char*)executor + one_executor);

    /* These threads will be started, but will instantly get put to sleep */
    for(unsigned int i; i < num; i++) {

      executor->ex_id = i + 1;
      executor->ex_next = NULL;
      executor->ex_kse_mbox.km_version = KSE_VER_0;
      executor->ex_kse_mbox.km_curthread = NULL;
      executor->ex_kse_mbox.km_completed = NULL;
      sigemptyset(&(executor->ex_kse_mbox.km_sigscaught));
      executor->ex_kse_mbox.km_flags = 0;
      executor->ex_kse_mbox.km_stack = (char*)executor + executor_space;
      executor->ex_kse_mbox.km_func = executor_scheduler;
      executor->ex_kse_mbox.km_udata = executor;

      if(kse_create(&(executor->ex_kse_mbox), 0))
	panic("Failed to initialize executor");

      executor = (executor_t*)((char*)executor + one_executor);

    }

    /* Create the main thread and dive into it */
    /* XXX actually create the main thread */
    /* XXX put it into my scheduler */
    executor->ex_kse_mbox.km_curthread = main_thread->t_os_data;

    if(kse_switchin(main_thread->t_os_data, 0))
      panic("Runtime error occurred in scheduler:\n");

  }

  else
    panic("Unable to allocate space for executor stacks:");

}


/*!
 * This function terminates all executors, destroys all schedulers and
 * threads, and shuts down the system.  When it returns, all system
 * resources consumed by executors are released, and the executors are
 * destroyed except the caller.  This action is irrevocable and should
 * be a precursor to terminating the runtime program itself.
 *
 * \brief Shut down the system, terminating and joining all executors.
 */
internal noreturn void executor_stop(void) {

}
