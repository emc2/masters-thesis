/* Copyright (c) Eric McCorkle 2007.  All rights reserved. */

#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "cc_stat.h"
#include "cc/thread.h"


/*!
 * This function modifies the initial parameters of the executors, and
 * returns the amount of static memory which will be necessary to
 * initialize them.
 *
 * \brief Set parameters and request static memory.
 * \arg stat The initial parameters, which will be modified if they
 * are unacceptable or 0.
 * \return The size of memory required to initialize the executors.
 */
internal unsigned int executor_request(cc_stat_t* restrict stat);


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
 * \arg argc The value of argc for the guest program.
 * \arg argv The value of argv for the guest program.
 * \arg envp The value of envp for the guest program.
 * \arg mem The static memory to use to initialize the system.
 */
internal noreturn void executor_start(unsigned int num, unsigned int stack_size,
				      void (*main) (thread_t* restrict thread,
						    unsigned int exec,
						    unsigned int argc,
						    const char* const * argv,
						    const char* const * envp),
				      unsigned int argc,
				      const char* const * argv,
				      const char* const * envp,
				      void* mem);

/*!
 * This function terminates all executors, destroys all schedulers and
 * threads, and shuts down the system.  When it returns, all system
 * resources consumed by executors are released, and the executors are
 * destroyed except the caller.  This action is irrevocable and should
 * be a precursor to terminating the runtime program itself.
 *
 * \brief Shut down the system, terminating and joining all executors.
 * \arg exec The ID of the executor running this.
 */
internal noreturn void executor_stop(unsigned int exec);


/*!
 * This function returns the number of executors in use.  This is not
 * presently able to be changed, so it will always return the same
 * value.  However, this is made a function so as to allow future
 * changes.
 *
 * \brief Get the number of executors in the system.
 * \return The number of executors in the system.
 */
internal unsigned int executor_count(void);


/*!
 * This function executes the same safepoint check code that should be
 * executed by user space.  This implements the entire "safepoint"
 * operation, as per the theoretical model.
 *
 * This is present to allow external code to execute safe points.
 *
 * \brief Execute a safepoint check
 * \arg exec The ID of the executor running this.
 * \arg sigs These signals are forced to be executed, regardless of
 * the mailbox state.
 */
internal void executor_safepoint(unsigned int exec, unsigned int sigs);

/*!
 * This function is the entry point for the scheduling system.  This
 * cycles the scheduler, and switches to the new thread.  This
 * implements the cases of the "safepoint" statement in the
 * theoretical model where the executor's mailbox flag is set.
 *
 * This is present to allow safe points to be implemented within the
 * language in which the hosted program is written.
 *
 * \brief The scheduler function for an executor.
 * \arg exec The ID of the executor running this.
 */
internal noreturn void executor_sched_cycle(unsigned int exec);

/*!
 * This function sends a signal to the given executor.  Multiple
 * signals can be sent by or'ing together several of the exec_signal_t
 * values.  Signals are not registered immediately, but at the next
 * safe-point.
 *
 * This implements the "raise" statement in the underlying theoretical
 * model.
 *
 * \brief Send a signal to a given executor.
 * \arg exec The executor to which to send the signal.
 * \arg sigs The set of signals to send.
 */
internal void executor_raise(unsigned int exec, unsigned int sigs);

/*!
 * This function returns the ID of the current executor.  This should
 * be used only to implement functions like malloc, which are required
 * to present a certain interface, and cannot take the executor ID as
 * an argument.  All functions presented by the runtime should just
 * take the executor ID as an argument.
 *
 * \brief Get the id of the current executor.
 * \return The id of the current executor.
 */
internal unsigned int executor_self(void);


/*!
 * This function attempts to restart a single idle executor to consume
 * threads which have been placed in workshare, or that have just been
 * created.  Note that the restarted executor will immediately
 * schedule, and will call this function again if there are still
 * threads on the workshare.
 *
 * This implements the "signal" statement in the underlying
 * theoretical model.
 *
 * \brief Attempt to restart idle executors.
 */
internal void executor_restart_idle(void);

#endif
