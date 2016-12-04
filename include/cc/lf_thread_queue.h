/* Copyright (c) 2007 Eric McCorkle.  All rights reserved. */

#ifndef LF_THREAD_QUEUE_H
#define LF_THREAD_QUEUE_H

#include "definitions.h"
#include "atomic.h"
#include "cc/thread.h"

/*!
 * This is the type of a lock-free thread queue.  This is a
 * limited-capacity queue.
 *
 * \brief Type of a fifo queue.
 */
typedef struct lf_thread_queue_t lf_thread_queue_t;

/*!
 * This is a per-thread hazard pointer structure (see Hazard Pointers:
 * Safe Memory Reclamation of Lock-Free Objects, Michael, 2004).
 *
 * \brief Type of a per-thread hazard pointer structure.
 */
typedef struct lf_thread_queue_hazard_ptrs_t lf_thread_queue_hazard_ptrs_t;

/*!
 * This is the type of a queue node.  Because of the lock-free nature
 * of these queues, it is necessary to create a separate node type for
 * queue nodes.  Otherwise, it would be preferable to use the threads
 * themselves.
 *
 * As a corollary, no attempt should be made to "fix" this, unless a
 * lock-free algorithm that allows this is discovered.
 *
 * \brief Type of a queue node.
 */
typedef struct lf_thread_queue_node_t lf_thread_queue_node_t;

struct lf_thread_queue_node_t {

  /*!
   * This is the next node pointer.  This points to another
   * lf_thread_queue_node_t structure.  This pointer is volatile; be
   * careful!
   *
   * \brief The next node in the queue.
   */
  volatile atomic_ptr_t lfn_next;

  /*!
   * This is a pointer to the thread contained in this node.  This
   * pointer is effectively constant and restricted.
   *
   * \brief The data held by this node.
   */
  thread_t* lfn_data;

};

struct lf_thread_queue_hazard_ptrs_t {

  /*!
   * This is the number of nodes in the retired list for this executor.
   * They will be periodically cleared out.  This is executor-local.
   *
   * \brief The number of nodes in the thp_rlist.
   */
  unsigned int thp_rcount;

  /*!
   * This is the retired list.  This holds pointers to nodes until
   * they are retired.  This is a executor-local structure.
   *
   * \brief The retired list for the current executor.
   */
  lf_thread_queue_node_t* thp_rlist;

  /*!
   * These are the hazard pointers for this executor.  If non-null, they
   * denote hazards, which must not be freed.  These are multi-reader,
   * single-writer.
   *
   * \brief The hazard pointers for this thread.
   */
  volatile atomic_ptr_t thp_ptrs[2];

};

/*!
 * This is a node list, which consists of the actual list an the total
 * number of nodes it contains.  This structure is contained in a
 * cache-line.
 *
 * \brief A node list.
 */
typedef union {

  struct {

    lf_thread_queue_node_t* _nl_list;
    unsigned int _nl_count;

  } value;

  cache_line_t _;

} lf_thread_queue_node_list_t;

#define nl_list value._nl_list
#define nl_count value._nl_count

/*!
 * This is a static node allocator.  A predetermined number of nodes
 * are statically created, and ollocated using this structure.  The
 * structure uses private lists, and a public node list to maintain
 * balance.
 *
 * \brief A node allocator for the queue.
 */
typedef struct {

  /*!
   * This is the shared list, as well as the total number of nodes.
   * The number of nodes is the total number, not the number in the
   * shared list.  It does not change.  This structure is aligned to a
   * cache line.
   *
   * \brief The shared list, and the total number of nodes.
   */
  union {

    struct {

      void* volatile _qna_shared_list;
      unsigned int _qna_count;

    } value;

    cache_line_t _;

  } _qna_shared;

  /*!
   * These are the per-executor node lists.  These are only accessed
   * by the executor which owns them.
   *
   * \brief The node lists for every executor.
   */
  lf_thread_queue_node_list_t qna_lists[];

} lf_thread_queue_node_allocator_t;

#define qna_shared_list _qna_shared.value._qna_shared_list
#define qna_count _qna_shared.value._qna_count

struct lf_thread_queue_t {

  /*!
   * This is a pointer to the node allocator.  This will be located
   * immediately following the hazard pointers.
   *
   * \brief A pointer to the allocator.
   */
  lf_thread_queue_node_allocator_t* lf_allocator;

  /*!
   * This is the head pointer of the fifo queue.  Deletes happen from
   * here.  This holds pointers to lf_thread_queue_node_t's.  This
   * pointer is volatile, be careful!
   *
   * \brief The head of the fifo.
   */
  volatile atomic_ptr_t lf_head;

  /*!
   * This is the tail pointer of the fifo queue.  Inserts go here.
   *   This holds pointers to lf_thread_queue_node_t's.  This pointer
   *   is volatile, be careful!
   *
   * \brief The tail of the fifo.
   */
  volatile atomic_ptr_t lf_tail;

  /*!
   * This is an array of per-thread hazard pointer structures.  There
   * are as many of these are there are executors operating on the
   * lock-free queue.
   *
   * \brief Per-thread hazard_ptrs structures.
   */
  lf_thread_queue_hazard_ptrs_t lf_hptrs[];

};


/*!
 * This function calculates and returns the amount of memory which
 * will be required to maintain a single fifo, with the specified
 * capacity and number of executors.
 *
 * \brief Request static memory for a single queue.
 * \arg num The capacity of the queue.
 * \arg execs The number of executors.
 * \return The size of memory required for a queue of the specified size.
 */
internal pure unsigned int lf_thread_queue_request(unsigned int num,
						   unsigned int execs);


/*!
 * This function creates a new, empty fifo queue which is capable of
 * holding num items.  It expects an amount of memory returned by
 * lf_thread_queue_request.
 *
 * \brief Create a fifo queue.
 * \arg mem A pointer to a block as large as was returned by
 * lf_thread_queue_request given the same parameters.
 * \arg num The capacity of the queue.
 * \arg execs The number of executors.
 * \return The newly created queue.
 */
internal lf_thread_queue_t* lf_thread_queue_init(void* mem,
						 unsigned int num,
						 unsigned int execs);

/*!
 * This function destroys the fifo passed in and reclaims its resources.
 *
 * \brief Destroy a fifo.
 * \arg fifo The fifo to be destroyed.
 * \arg exec The ID of the calling executor.
 */
internal void lf_thread_queue_destroy(lf_thread_queue_t* restrict fifo,
				      unsigned int exec);


/*!
 * This function enqueues thread onto fifo.  This function operates
 * atomically on the fifo, and can be called by any number of threads.
 * This may fail if there are no nodes available.
 *
 * \brief Enqueue a thread.
 * \arg fifo The fifo onto which to enqueue.
 * \arg exec The ID of the calling executor.
 * \arg thread The thread to enqueue.
 * \return Whether or not the call succeeded (a node was available).
 */
internal bool lf_thread_queue_enqueue(lf_thread_queue_t* restrict fifo,
				      thread_t* restrict thread,
				      unsigned int exec);

/*!
 * This function dequeues a thread from fifo and returns it.  This
 * function operates atomically on fifo.
 *
 * \brief Dequeue a thread.
 * \arg fifo The fifo from which to dequeue.
 * \arg exec The ID of the calling executor.
 * \return The dequeued thread or NULL if no thread is available.
 */
internal thread_t* lf_thread_queue_dequeue(lf_thread_queue_t* restrict fifo,
					   unsigned int exec);

#endif
