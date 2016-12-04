/* Copyright (c) 2007, 2008 Eric McCorkle.  All rights reserved. */

#ifndef GC_THREAD_H
#define GC_THREAD_H

#include "definitions.h"
#include "mm/gc_desc.h"

#include <stdbool.h>

/*!
 * This is the number of entries in a thread's write log.
 *
 * \brief The size of a write log.
 */
#define GC_WRITE_LOG_SIZE 64

/*!
 * This is the minimum number of elements for an array to be processed
 * in clusters.  If an array has fewer elements, it will be processed
 * as a single object.
 *
 * \brief The number of elements threshold for array clusterization.
 */
#define GC_ARRAY_MIN_COUNT 64

/*!
 * This is the minimum size for array clusterization.  If the array is
 * smaller, it will not be clusterized, but will be processed as a
 * single object.
 *
 * \brief The minimum array size for clusterization
 */
#define GC_ARRAY_MIN_SIZE 4096

/*!
 * This is the size of an array cluster.  Arrays will be broken up
 * into groups of this many elements and processed as a block.
 *
 * \brief The size threshold for array clusterization
 */
#define GC_ARRAY_CLUSTER_SIZE 16


/*!
 * This is a single allocator.  One such allocator exists for each
 * generation, for each executor.  Additionally, the garbage collector
 * maintains its own allocators, which it uses to allocate memory
 * while building the new heap.
 *
 * The first pointer is the allocation pointer.  This pointer is
 * advanced with each allocation.  The second is the limit pointer.
 * This points to the first byte after the end of the block.
 *
 * \brief A single allocator.
 */
typedef void* gc_allocator_t[2];


/*!
 * This is a hash node used to process write log entries.  This helps
 * avoid processing the same location multiple times.
 *
 * \brief A hash node used to process write log entries.
 */
typedef struct gc_write_log_hash_node_t {

  /*!
   * This is the next pointer for a list of unique locations.  The
   * node in inserted into the hash table and the unique list only
   * once.
   *
   * \brief The next pointer for the unique list.
   */
  struct gc_write_log_hash_node_t* wh_list_next;

  /*!
   * This is the next pointer for the bucket lists in a hash table.
   * The node in inserted into this table, and subsequent inserts are
   * ignored.
   *
   * \brief The next pointer for the hash table.
   */
  struct gc_write_log_hash_node_t* wh_hash_next;

  /*!
   * This is the write log entry whose address is represented by this
   * node.
   *
   * \brief The log entry.
   */
  volatile gc_log_entry_t* wh_log_entry;

} gc_write_log_hash_node_t;

/*!
 * This is the closure for a garbage collection thread.  This can be
 * thought of as thread-local data.  A pointer to this structure is
 * passed as a parameter to the gc_thread function.
 *
 * \brief A garbage collection thread's closure.
 */
typedef struct {

  /*!
   * This is a special counter which controls action on the workshare
   * queue.  Because keeping a total count of all objects presently
   * known to the collector is not feasible, the upper and lower
   * bounds on the number of objects in one gc_closure's queue has to
   * be constant.  This will create a situation of high contention on
   * the queue in many cases.  Therefore, this counter is used to
   * reduce the number of accesses to the workshare.
   *
   * The counter represents an estimate of how many objects the thread
   * needs to add or remove from the workshare to be balanced
   * (positive values represent the need to add, negative values
   * represent the need to remove).  A value of the workshare queue
   * capacity / number of executors is used as moderator value.
   * Threads begin collection with a count of +moderator.  The
   * count has a range of -moderator to +moderator.  If a pull fails,
   * the count is reset to +moderator.  If a push fails, the counter
   * is reset to -moderator.  If a pull fails, it is reset to
   * +moderator.  Otherwise, everytime the thread intends to push, it
   * decrements, and everytime it intends to pull, it increments.
   *
   * Which action the thread intends to do, and whether or not it
   * actually attempts it is determined from the count and the number
   * of items in the local queue.  The value count + (moderator -
   * local) is calculated.  If this value is negative, the thread will
   * intend to pull.  If it is positive, the thread will intend to
   * push.  The raw count is used in the same way to determine whether
   * or not the action is actually done.  Regardless, if the thread
   * intends to pull, it increments count, and if it intends to push,
   * it decrements it.
   *
   * \brief A counter for the next action on the workshare queue.
   */
  int gth_workshare_count;

  /*!
   * This is the number of blocks in the local block queue.
   *
   * \brief The number of blocks in the queue.
   */
  unsigned int gth_queue_size;

  /*!
   * This is the head of the local queue.  Dequeues happen here.
   *
   * \brief The head of the local queue.
   */
  volatile void* gth_head;

  /*!
   * This is the tail of the local queue.  Enqueues happen here.
   *
   * \brief The tail of the local queue.
   */
  volatile void* gth_tail;

  /*!
   * This is the head of a list used to determine all the unique
   * locations affected by a given write log.
   *
   * \brief Head of a list used to process write logs.
   */
  gc_write_log_hash_node_t* gth_unique_list;

  /*!
   * This is a collection of hash nodes used to process write logs.
   * This avoids multiple processing of a given location.  These are
   * pre-allocated to avoid calling malloc.
   *
   * \brief Hash nodes for processing write logs.
   */
  gc_write_log_hash_node_t gth_hash_nodes[GC_WRITE_LOG_SIZE];

  /*!
   * This is a pre-allocated hash table used for processing write
   * logs.  This helps avoid processing the same location multiple
   * times.
   *
   * \brief Hash table for processing write logs.
   */
  gc_write_log_hash_node_t* gth_hash_table[1024];

  /*!
   * There are the allocators for this garbage collection thread.
   * There is one allocator for each generation.
   *
   * \brief The allocators for this thread.
   */
  gc_allocator_t gth_allocators[];

} gc_closure_t;


/*!
 * This function calculates the size of memory required by garbage
 * collector thread structures.  Some structures' sizes cannot be
 * determined at compile time, and a slice must be allocated for them
 * at runtime.  This function facilitates this.
 *
 * \brief Calculate memory required by collector threads.
 * \arg execs The number of executors.
 * \arg gens The number of generations.
 * \return The size of memory required by collector threads.
 */
internal unsigned int gc_thread_request(unsigned int execs,
					unsigned int gens);

/*!
 * Initialize garbage collection thread structures for some number of
 * executors.  This sets up the necessary structures for the garbage
 * collection threads.  This must be called before any GC-allocation
 * is attempted.
 *
 * \brief Setup garbage collection threads.
 * \arg execs The number of executors.
 * \arg mem The statically allocated memory available to the
 * subsystem.
 * \return The new free space.
 */
internal void* gc_thread_init(unsigned int execs, unsigned int num_gen,
			      void* restrict mem);

/*!
 * This function initializes the gc_thread_t structure corresponding
 * to a given executor.  This is to be called from within the
 * executor's initialization procedures.
 *
 * \brief Initialize a gc_thread for a given executor.
 * \arg thread The gc_thread_t to initialize.
 * \arg log The write log that will be processed by this GC thread.
 */
internal void gc_closure_init(gc_closure_t* closure,
			      volatile gc_log_entry_t* log);

/*!
 * This function activates the garbage collector threads, starting the
 * allocation.  The garbage collector state must be set to an active
 * state prior to calling this function.  The collectors will run once
 * started, until they complete, at which point they will modify
 * gc_state accordingly.

 * \brief Activate the garbage collectors.
 * \arg gen The highest generation to collect.
 */
internal void gc_thread_activate(unsigned char gen);


/*!
 * This function is the entry function for garbage collector threads.
 * This thread does not require a separate stack.  It is designed so
 * that it can be executed from the beginning each time.
 *
 * \brief Entry function for garbage collector threads.
 */
internal void gc_thread(gc_closure_t* restrict closure, unsigned int exec);

#endif
