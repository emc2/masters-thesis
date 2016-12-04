/* Copyright (c) Eric McCorkle 2007.  All rights reserved. */

#ifndef THREAD_H
#define THREAD_H

#include "atomic.h"
#include "mm/gc_desc.h"
#include "mm/gc_alloc.h"
#include "cc/context.h"

/*!
 * This is the enumeration type for thread status.  Thread status is
 * used to request changes.  Only set the status to T_STAT_RUNNABLE,
 * T_STAT_SUSPEND, T_STAT_TERM.  The other statuses indicate
 * acknowledegement from the scheduler system.  T_STAT_DESTROY is
 * identical to T_STAT_TERM, but the thread is destroyed when it is
 * "acknowledged".  T_STAT_NONE is a code used to indicate that the
 * status should not be changed.
 *
 * Once a thread enters T_STAT_TERM status, it may only enter
 * T_STAT_DEAD, or T_STAT DESTROY status.  T_STAT_DEAD and
 * T_STAT_DESTROY are both "dead ends".
 *
 * Some other statuses are purely internal, for the garbage collector.
 * There inclide T_STAT_GC, T_STAT_GC_WAIT, T_STAT_FINALIZER_WAIT, and
 * T_STAT_FINALIZER_LIVE
 *
 * The T_STAT_GC status is for a collector thread.  This status is
 * active when the collector is running and inactive otherwise.
 *
 * The T_STAT_GC_COLLECTOR status is for collector threads.  Counts as
 * T_STAT_RUNNING if a collection is running, and T_STAT_SUSPENDED
 * otherwise.
 *
 * The T_STAT_GC_WAIT status is for collectors waiting on the
 * collection to finish.  If the scheduler encounters such a thread
 * and the collector is not active, it will set its status to
 * T_STAT_RUNNING.
 *
 * The T_STAT_FINALIZER_WAIT status is identical to T_STAT_SUSPENDED,
 * except that it refers to finalizers which are waiting to wake up.
 * The T_STAT_FINALIZER_LIVE status is used to mark finalizers for
 * waking up.  Once the finalizer does wake up, it will be set to
 * T_STAT_RUNNING.
 *
 * \brief The enumeration for thread status.
 */
typedef enum thread_sched_stat_t thread_sched_stat_t;

enum thread_sched_stat_t {
  T_STAT_RUNNABLE = 0x0,
  T_STAT_RUNNING = 0x1,
  T_STAT_SUSPEND = 0x2,
  T_STAT_SUSPENDED = 0x3,
  T_STAT_TERM = 0x4,
  T_STAT_DEAD = 0x5,
  T_STAT_DESTROY = 0x6,
  T_STAT_GC_WAIT = 0x7,
  T_STAT_FINALIZER_LIVE = 0x8,
  T_STAT_FINALIZER_WAIT = 0x9,
  T_STAT_NONE = 0xf
};

#define T_STAT_MASK 0xf
#define T_REF 0x10


/*!
 * This code denotes that no executor is currently running the thread
 * when this is present in the thread_mbox_t structure's
 * thread_mbox_executor field.
 *
 * Note: This cannot be trusted to be a reliable indicator, as it
 *
 * \brief A code to denote that no executor is running the thread.
 */
extern const unsigned int thread_mbox_null_executor;

/*!
 * This structure is a type for user-visible thread status.  This
 * includes the current executor, the current executor stack, the
 * current scheduling state, the thread's ID, and the hard priority
 * (which has meaning only on interactive schedulers).
 *
 * \brief A type for user-visible thread status.
 */
typedef char thread_mbox_t[sizeof(void*) + sizeof(void*) +
			   sizeof(unsigned int) + sizeof(void*) +
			   sizeof(unsigned int) + sizeof(void*) +
			   sizeof(void*)];

typedef void (*retaddr_t)(void);

/*!
 * This function gets a pointer to the return address slot in the
 * mailbox.
 *
 *  This is the offset at which the thread stores its own return
 * address when entering the runtime.
 *
 * \brief Get a pointer to the return address.
 * \arg mbox The mailbox for which to get an offset.
 * \return A pointer to the return address in the mailbox.
 */
internal pure volatile retaddr_t*
thread_mbox_retaddr(volatile thread_mbox_t mbox);

/*!
 * This function gets a pointer to the stack pointer slot in the
 * mailbox.
 *
 * This is the offset of the stack pointer (aka frame) for the thread.
 * While running in the guest program, this will hold the address of
 * the executor stack.  When entering the runtime, the program must
 * save its return address, and the address of its current frame here.
 * All other machine state is assumed to be wiped out during the call
 * to the runtime.
 *
 * \brief Get a pointer to the stack pointer.
 * \arg mbox The mailbox for which to get an offset.
 * \return A pointer to the stack pointer in the mailbox.
 */
internal pure volatile void* volatile *
thread_mbox_stkptr(volatile thread_mbox_t mbox);


/*!
 * This function gets a pointer to the executor ID slot in the mailbox.
 *
 * While in the guest program, the ID of the executor is stored here.
 * The runtime must change this whenever the thread is rescheduled to
 * a different executor.
 *
 * \brief Get a pointer to the executor ID.
 * \arg mbox The mailbox for which to get an offset.
 * \return A pointer to the executor ID in the mailbox.
 */
internal pure volatile unsigned int*
thread_mbox_executor(volatile thread_mbox_t mbox);


/*!
 * This function gets a pointer to the signal pointer slot in the
 * mailbox.
 *
 * While in the guest program, a pointer to the executor's signal
 * mailbox is stored here.  This must also be updated whenever the
 * thread is rescheduled.  The guest program must use this pointer to
 * check for signals at safe-points.
 *
 * \brief Get a pointer to the signal pointer.
 * \arg mbox The mailbox for which to get an offset.
 * \return A pointer to the signal pointer in the mailbox.
 */
internal pure volatile atomic_uint_t* volatile *
thread_mbox_sigptr(volatile thread_mbox_t mbox);


/*!
 * This function gets a pointer to the gc write log index slot in the
 * mailbox.
 *
 * This is the next valid index in the gc write log.  If this is equal
 * to GC_WRITE_LOG_LENGTH, then the write log is full.
 *
 * \brief Get a pointer to the write log index.
 * \arg mbox The mailbox for which to get an offset.
 * \return A pointer to the write log index in the mailbox.
 */
internal pure volatile unsigned int*
thread_mbox_write_log_index(volatile thread_mbox_t mbox);


/*!
 * This function gets a pointer to the gc write log slot in the
 * mailbox.
 *
 * This points to the gc write log for the executor running the
 * thread.  All writes to GC memory must be logged in the write log.
 *
 * \brief Get a pointer to the write log.
 * \arg mbox The mailbox for which to get an offset.
 * \return A pointer to the write log in the mailbox.
 */
internal pure volatile gc_log_entry_t* volatile *
thread_mbox_write_log(volatile thread_mbox_t mbox);


/*!
 * This function gets a pointer to the gc allocator slot in the
 * mailbox.
 *
 * This points to an array of gc_num_generations + 1 gc_allocator_t
 * structures, which are to be used for allocation by this executor.
 *
 * \brief Get a pointer to the write log.
 * \arg mbox The mailbox for which to get an offset.
 * \return A pointer to the write log in the mailbox.
 */
internal pure volatile gc_allocator_t* volatile *
thread_mbox_allocators(volatile thread_mbox_t mbox);


/*!
 * This structure contains user-visible and modifiable thread data.
 * This structure is used to set or retrieve information about a
 * particular thread from the runtime.
 *
 * \brief The thread status structure.
 */
typedef struct thread_stat_t thread_stat_t;

/*!
 * This is the type of a user-level thread.  These threads are mapped
 * onto execution resources, called executors by the scheduler.  These
 * structures may alter depending on the implementation of the runtime
 * system.
 *
 * \brief Type of a user-level thread.
 */
typedef struct thread_t thread_t;

struct thread_stat_t {

  /*!
   * This is the numerical ID of the thread.  This is unique among
   * threads, but the ordering of ID's is indeterminant.  This field
   * is read-only.
   *
   * \brief The thread's ID.
   */
  unsigned int t_id;

  /*!
   * This is the scheduling status for the thread.  The statuses
   * T_STAT_RUNNING, T_STAT_SUSPENDED, and T_STAT_DEAD denote
   * acknowledgement by the scheduler, but are functionally equivalent
   * to their counterparts T_STAT_RUNNABLE, T_STAT_SUSPEND, and
   * T_STAT_TERM.  Note that a thread can only be set to the latter
   * trio externally.  Only the scheduler can set its status to the
   * former.
   *
   * \brief The scheduling status for the thread.
   */
  thread_sched_stat_t t_sched_stat;

  /*!
   * This is the "hard" priority.  This is the priority of the thread
   * as given by users.  This has no meaning in a non-interactive
   * implementation.
   *
   * \brief The user-assigned priority of the thread.
   */
  unsigned int t_pri;

  /*!
   * This is the destructor for this thread.  If this function pointer
   * is not null, it will be called with the thread as an argument
   * when the thread is finally destroyed.  For threads allocated with
   * malloc, this should be free.
   *
   * \brief The destructor.
   */
  void (*t_destroy)(thread_t* thread);

};

struct thread_t {

  /*!
   * This is the scheduling state for the thread.  This is actually an
   * instance of a thread_sched_stat_t enumeration value, and as such,
   * may only be one of T_STAT_RUNNABLE, T_STAT_RUNNING,
   * T_STAT_SUSPEND, T_STAT_SUSPENDED, T_STAT_TERM, or T_STAT_DEAD.
   * This value is volatile, and may change spontaneously if observed
   * by anyone.  Scheduling state change does not necessarily take
   * effect immediately, and is not guaranteed to have taken effect
   * until the state is observed to change from one of
   * T_STAT_RUNNABLE, T_STAT_SUSPEND, or T_STAT_TERM to a
   * corresponding one of T_STAT_RUNNING, T_STAT_SUSPENDED, or
   * T_STAT_DEAD.
   *
   * This is also a reference flag T_REF for references within the
   * scheduler's internal structures.  This is present to prevent
   * double entries from appearing in the data structures.  If a
   * CAS(anything & ~T_REF, T_REF | T_STAT_RUNNABLE, ...)  succeeds,
   * the thread must then be inserted into the workshare queue.
   * Whenever a thread is observed to have a non-runnable status, it
   * must be acknowledged with a CAS(stat, stat & ~T_REF, ...).
   * Lastly, if T_STAT_DESTROY is ever observed, the thread must be
   * destroyed.
   *
   * These must be packed together to allow simultaneous atomic
   * compare-and-set operations.
   *
   * \brief The scheduling state for the thread, as well as the
   * count of references made by internal scheduler structures.
   */
  volatile atomic_uint_t t_sched_stat_ref;

  /*!
   * This is a pointer to the thread's mailbox structure.  This is a
   * structure which resides in the guest program, and is used to
   * confer information from the runtime to the guest.
   *
   * \brief The thread's mailbox structure.
   */
  volatile thread_mbox_t t_mbox;

  /*!
   * This is the ID value for this thread.  This is guaranteed to be
   * unique for all threads.
   *
   * \brief The ID for this thread.
   */
  unsigned int t_id;

#ifdef INTERACTIVE
#error "Interactive scheduler not implemented"

  /*!
   * This is the "hard" priority.  This is the priority of the thread
   * as given by users.  This has no meaning in a non-interactive
   * implementation.
   *
   * \brief The user-assigned priority of the thread.
   */
  unsigned int t_hard_pri;

  /*!
   * This is the "soft" priority.  This is the temporary boost in
   * priority given to the thread by the scheduler system for holding
   * locks, et cetera.
   *
   * \brief The scheduler-assigned priority of the thread.
   */
  unsigned int t_soft_pri;

#endif

  /*!
   * This is the destructor for this thread.  If this function pointer
   * is not null, it will be called with the thread as an argument
   * when the thread is finally destroyed.  For threads allocated with
   * malloc, this should be free.
   *
   * \brief The destructor.
   */
  void (*t_destroy)(thread_t* thread);

  thread_t* t_rlist_next;
  thread_t* t_queue_next;

};


/*!
 * This function initializes a thread structure, preparing it for use.
 * The mailbox that is passed in is copied into the thread's mailbox.
 * It is not used once the call ends, so it can be freed, reused, et
 * cetera safely.
 *
 * \brief Initialize a thread structure.
 * \arg thread The thread to initialize.
 * \arg stat The initial status structure for the thread.
 * \arg mbox The initial mailbox state for the thread (sets the entry
 * function and stack pointer).
 */
internal void thread_init(thread_t* restrict thread,
			  const thread_stat_t* restrict stat,
			  const thread_mbox_t mbox);

/*!
 * This function destroys a thread, releasing its resources.  This
 * should only be called after the thread has terminated.
 *
 * \brief Destroy a thread structure.
 * \arg thread The thread to destroy.
 */
internal void thread_destroy(thread_t* restrict thread);


/*!
 * This function gets the current number of threads.  The result may
 * change sporadically.
 *
 * \brief Get the current number of threads.
 * \return The number of threads.
 */
internal unsigned int thread_count(void);

#endif
