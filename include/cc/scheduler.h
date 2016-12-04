/* Copyright (c) 2007, 2008 Eric McCorkle.  All rights reserved. */

#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "definitions.h"
#include "cc/thread.h"
#include "cc/lf_thread_queue.h"

/*!
 * This is the type of a single thread's scheduler data.  This
 * structure may change between the interactive and non-interactive
 * implementations.
 *
 * \brief The type of a single thread's scheduler.
 */
typedef struct scheduler_t scheduler_t;

struct scheduler_t {

#ifdef INTERACTIVE
#error "Interactive scheduler not implemented"
#else

  /*!
   * This is the number of threads currently assigned to this
   * scheduler.
   *
   * \brief The number of threads assigned to this scheduler.
   */
  unsigned int sch_num_threads;

  /*!
   * This is the the head of the private queue for this scheduler.
   * This is not a lock-free queue, but a simple FIFO using the next
   * pointers in the thread_t structure.  Deletes come from here.
   *
   * \brief The head of the private queue for this scheduler
   */
  thread_t* sch_queue_head;

  /*!
   * This is the the tail of the private queue for this scheduler.
   * This is not a lock-free queue, but a simple FIFO using the next
   * pointers in the thread_t structure.  Inserts go here.
   *
   * \brief The tail of the private queue for this scheduler
   */
  thread_t* sch_queue_tail;

  /*!
   * This is the current thread.  It does not reside in the scheduler
   * queue.
   *
   * \brief The current thread.
   */
  thread_t* sch_curr_thread;

  /*!
   * This is the idle thread for this scheduler.  It waits for more
   * threads to appear, and then surrenders control.
   *
   * \brief The idle thread.
   */
  thread_t* restrict sch_idle_thread;

  /*!
   * This is the garbage collector thread for this executor.  This
   * thread will only be run when the collection is running.  It's
   * status is perpetually T_STAT_GC_COLLECTOR.
   *
   * \brief The GC collector thread.
   */
  thread_t* restrict sch_gc_thread;


#endif

};


/*!
 * This function modifies the initial parameters of the threading
 * system, and returns the amount of static memory which will be
 * necessary to initialize it.
 *
 * \brief Set parameters and request static memory.
 * \arg stat The initial parameters, which will be modified if they
 * are unacceptable or 0.
 * \return The size of memory required to initialize the threading
 * system.
 */
extern unsigned int scheduler_request(const cc_stat_t* restrict stat);


/*!
 * This function initializes the user thread scheduling system as a
 * whole.  This must be called before any individual schedulers are
 * allocated and initialized.
 *
 * \brief Initialize the scheduler system itself.
 * \arg execs The number of executors.
 * \arg mem The static memory to use to initialize the system.
 */
internal void* scheduler_start(unsigned int execs, void* mem);


/*!
 * This function deactivates and destroys the scheduler system itself.
 * It should not be called while any threads are still active, as it
 * will wait for all scheduler operations to reach a quiescent state
 * before continuing, which will not happen if threads are still
 * running.
 *
 * \brief Shut down the scheduler system itself.
 * \arg exec The ID of the calling executor.
 */
internal void scheduler_stop(unsigned int exec);


/*!
 * This function performs any initialization requiring allocation of
 * space for the given scheduler.  It does not perform any additional
 * initialization, and the scheduler will not be ready for use until
 * scheduler_init has been called on it.
 *
 * \brief Allocate structures for a scheduler structure.
 * \arg scheduler The scheduler to initialize.
 * \arg mem The static memory to use to initialize the system.
 * \return The next available static memory.
 */
internal void* scheduler_setup(scheduler_t* restrict scheduler, void* mem);


/*!
 * This function completes initialization of a scheduler.  Once this
 * has been called, a scheduler is properly initialized.
 *
 * \brief Initialize a scheduler structure.
 * \arg scheduler The scheduler to initialize.
 * \arg idle_thread A pointer to the idle thread for this scheduler.
 * \arg gc_thread A pointer to the GC thread for this scheduler.
 */
internal void scheduler_init(scheduler_t* restrict scheduler,
			     thread_t* restrict idle_thread,
			     thread_t* restrict gc_thread);

/*!
 * This function destroys the scheduler structure, releasing its
 * resources.  The scheduler itself must be empty.
 *
 * \brief Destroy a scheduler.
 * \arg scheduler The scheduler to be destroyed.
 */
internal void scheduler_destroy(scheduler_t* restrict scheduler);


/*!
 * This function cycles the given scheduler once, possibly
 * descheduling a thread and running the next one.  The function
 * returns the thread to be executed (this may be the same thread that
 * was previously running in an interactive system).  The scheduler
 * will be updated accordingly.
 *
 * This will also check to see if the thread has become unrunnable,
 * and if it has, will drop it and cycle the scheduler once.
 *
 * In this scheduler implementation, this does not mutate the
 * scheduler at all.  It returns the running thread.  It should never
 * even be called to begin with in a non-interactive scheduler.
 *
 * If there is no thread available, sch_curr_thread is set to NULL,
 * while the idle thread is returned.
 *
 * \brief Run a scheduling cycle, returning the thread to be executed.
 * \arg scheduler The scheduler structure from which to schedule, and
 * which to update
 * \arg exec The ID of the current executor.
 * \return The thread to be executed this cycle (will not be NULL).
 */
internal thread_t* scheduler_cycle(scheduler_t* restrict scheduler,
				   unsigned int exec);


/*!
 * This function takes the current thread for scheduler off of the
 * scheduler, assiming it is no longer in a runnable state.  It then
 * cycles the scheduler once, and returns the resulting thread for
 * execution.  The scheduler will be updated accordingly.
 *
 * This assumes the thread state has already been set to some
 * unrunnable state.  It also assumes that the current thread is not
 * the idle thread.
 *
 * If there is no thread available, sch_curr_thread is set to NULL,
 * while the idle thread is returned.
 *
 * \brief Current thread becomes unrunnable, cycle once and return
 * next thread.
 * \arg scheduler The scheduler structure from which to schedule, and
 * which to update
 * \arg exec The ID of the current executor.
 * \return The thread to be executed this cycle, or NULL.
 */
internal thread_t* scheduler_replace(scheduler_t* restrict scheduler,
				     unsigned int exec);


/*!
 * This function activates the given thread, adding it to the
 * scheduler system.  The thread should already be ready to run, and
 * may not run immediately.
 *
 * This implements the "wake" statement in the underlying model.
 *
 * \brief Add thread to the scheduler system.
 * \arg thread The thread to activate.
 * \arg exec The ID of the current executor.
 * \return Whether or not the call succeeded.
 */
internal bool scheduler_activate_thread(thread_t* thread, unsigned int exec);


/*!
 * This function deactivates the given thread, effectively removing it
 * from the scheduler system.  The thread's status will be set to the
 * stat parameter, which should be a non-acknowledged, nonrunnable
 * state.
 *
 * This may be called on a running thread.  Doing so will not
 * necessarily cancel the thread immediately, but signal the executor
 * executing it to stop and reschedule.
 *
 * \brief Deactivate and remove a thread from the scheduler system.
 * \arg thread The thread to deactivate.
 * \arg stat The new status for the thread.
 * \arg exec The ID of the current executor.
 * \return Whether or not the call succeeded.
 */
internal bool scheduler_deactivate_thread(thread_t* thread,
					  thread_sched_stat_t stat,
					  unsigned int exec);

/*!
 * This function updates the given thread's status without having the
 * change take effect.  This is used to implement the sleep function
 * as defined by the underlying theoretical model.  This should never
 * be used to set a running status.
 *
 * \brief Update the thread's status without having it take effect.
 * \arg thread The thread to update.
 * \arg stat The thread's new status.
 * \return Whether or not the call succeeded.
 */
internal bool scheduler_update_thread(thread_t* thread,
				      thread_sched_stat_t stat);


/*!
 * This function gets the current number of active threads.  The
 * result may change sporadically.
 *
 * \brief Get the current number of active threads.
 * \return The number of threads.
 */
internal unsigned int sched_active_thread_count(void);

#endif
