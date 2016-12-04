/* Copyright (c) 2007, 2008 Eric McCorkle.  All rights reserved. */

#include <stdlib.h>
#include <string.h>
#include "definitions.h"
#include "program.h"
#include "gc.h"
#include "atomic.h"
#include "cc.h"
#include "os_thread.h"
#include "cc/executor.h"
#include "mm/gc_desc.h"
#include "mm/gc_vars.h"
#include "mm/gc_alloc.h"
#include "mm/gc_thread.h"
#include "mm/lf_object_queue.h"

typedef struct {

  unsigned char gc_gen;
  unsigned char gc_count;

} gen_count_t;

/* The static thread data, allocated in the beginning */
static unsigned int gc_thread_count;
static void* gc_global_ptr_bitmap;

/* The workshare queue, used to more evenly divide work. */
static lf_object_queue_t* gc_workshare;

/* This is the index at which to start claiming blocks of global
 * pointers to process.  Executors should claim some small number,
 * like 16, incrementing this value accordingly.
 */
static volatile atomic_uint_t gc_thread_data_index;

/* This is a lock-free simple stack of arrays.  Threads use the bitmap
 * which preceeds any array to claim blocks of array elements to
 * process.
 */
static volatile atomic_ptr_t gc_thread_array_list;

static barrier_t gc_thread_initial_barrier_value;
static barrier_t gc_thread_middle_barrier_value;
static barrier_t gc_thread_final_barrier_value;

/* These track the generation being collected.  The next generation is
 * updated when collection of a specific generation is requested.
 * Generations run in a sawtooth pattern.
 */
static volatile unsigned char gc_thread_last_gen = 1;
static volatile unsigned char gc_thread_peak_gen = 1;
static volatile atomic_uint_t gc_thread_next_gen;

volatile atomic_uint_t gc_state;
volatile uint64_t gc_collection_count = 0;

internal unsigned int gc_num_generations;

static const unsigned int gc_gen_promote = 2;

/* Algorithm overview:
 *
 * When a collector first encounters a block, either from a pointer
 * contained in a block from its queue, or from the root set, it tries
 * to compare-and-set its allocation pointer into the first word (the
 * forwarding pointer).  If it fails, then the collector goes on to
 * the next block.  If it succeeds, then the collector enqueues the
 * block.
 *
 * Collectors first traverse the entire root set attempting to claim
 * each block for themselves, then begin processing their own queues.
 * When a collector removes a thread from its queue, it performs a
 * snapshot copy of the block to the forwarding pointer, then attempts
 * to claim each of its heap pointers.
 *
 * Mutators must keep a write log of locations they have written while
 * a collection is in progress.  When this log fills up, a mutator
 * must stop and switch to the collector.  Each time a collector
 * thread resumes, it processes its own write log, possibly adding new
 * blocks to its queue.
 *
 * Collectors periodically share work by pushing blocks from their own
 * queues onto a lock-free structure, or pulling blocks off this
 * structure onto their own.
 *
 * When a collector exhausts its own queue, it sets a bit on a mask
 * and enters a sleep state.  If a collector moves work onto the
 * workshare queue and other collectors are sleeping, it awakens one
 * of them.  Additionally, if a collector's write log becomes
 * non-empty, the collector must be reactivated.  The last collector
 * to finish the collection switches over the state before it
 * deactivates itself.
 *
 * Collection happens in two phases.  In the first, threads which
 * allocate memory allocate from the old memory space only (unless
 * that becomes exhausted).  In the second, threads allocate from both
 * memory spaces.  In either case, threads must keep a write log for
 * as long as the collector is operating.
 */

/* XXX the lower bits of object pointers need to be masked */

internal unsigned int gc_thread_request(const unsigned int execs,
					const unused unsigned int gens) {

  PRINTD("  Reserving space for garbage collector\n");

  const unsigned int cache_line_bits = CACHE_LINE_SIZE * 8;
  const unsigned int gc_global_ptr_bitmap_size =
    (((gc_global_ptr_count - 1) & ~(cache_line_bits - 1))
     + cache_line_bits) / 8;
  const unsigned int queue_size = lf_object_queue_request(64 * execs, execs);

  PRINTD("    Reserving 0x%x bytes for global pointer bitmap.\n",
	 gc_global_ptr_bitmap_size);
  PRINTD("    Reserving 0x%x bytes for object queue.\n",
	 queue_size);
  PRINTD("  Garbage collector total static size is 0x%x bytes.\n",
	 queue_size + gc_global_ptr_bitmap_size);

  return queue_size + gc_global_ptr_bitmap_size;

}


internal void* gc_thread_init(const unsigned int execs,
			      const unused unsigned int gens,
			      void* const restrict mem) {

  PRINTD("Initializing GC system, static memory at 0x%p.\n", mem);

  const unsigned int cache_line_bits = CACHE_LINE_SIZE * 8;
  const unsigned int gc_global_ptr_bitmap_size =
    (((gc_global_ptr_count - 1) & ~(cache_line_bits - 1))
     + cache_line_bits) / 8;
  const unsigned int gc_thread_size =
    sizeof(gc_closure_t) + (sizeof(gc_allocator_t) * gens);
  const unsigned int gc_threads_size = execs * gc_thread_size;
  const unsigned int aligned_gc_threads_size =
    ((gc_threads_size - 1) & ~(CACHE_LINE_SIZE - 1)) + CACHE_LINE_SIZE;
  void* const bitmap = lf_object_queue_init(mem, execs * 64, execs);
  void* const out = (char*)bitmap + gc_global_ptr_bitmap_size;

  PRINTD("GC system memory:\n");
  PRINTD("\tobject queue at 0x%p\n", mem);
  PRINTD("\tglobal pointer bitmap at 0x%p\n", bitmap);
  PRINTD("\tend at 0x%p\n", out);

  os_thread_barrier_init(&gc_thread_initial_barrier_value, execs);
  os_thread_barrier_init(&gc_thread_middle_barrier_value, execs);
  os_thread_barrier_init(&gc_thread_final_barrier_value, execs);
  gc_thread_data_index.value = 0;
  gc_thread_count = execs;
  gc_workshare = mem;
  gc_global_ptr_bitmap = bitmap;
  memset(gc_global_ptr_bitmap, 0, gc_global_ptr_bitmap_size);

  return out;

}


internal void gc_thread_executor_init(gc_closure_t* const closure,
				      volatile gc_log_entry_t* const log) {

  closure->gth_workshare_count = 0;
  closure->gth_queue_size = 0;
  closure->gth_head = NULL;
  closure->gth_tail = NULL;
  closure->gth_unique_list = NULL;
  memset(closure->gth_hash_table, 0, 1024 * sizeof(gc_write_log_hash_node_t*));

  for(unsigned int i = 0; i < GC_WRITE_LOG_LENGTH; i++)
    closure->gth_hash_nodes[i].wh_log_entry = log + i;

}


static inline bool gc_thread_try_activate(void) {

  const unsigned int state = gc_state.value;
  const unsigned char old_phase = state & GC_STATE_PHASE;
  const unsigned char new_phase =
    GC_STATE_INACTIVE != old_phase ? old_phase : GC_STATE_INITIAL;
  bool out;

  /* Don't do anything if nothing changes, and nothing can be
   * changed once the state is no longer initial or inactive
   */
  if(new_phase != old_phase) {

    const unsigned int new_state = (state & ~GC_STATE_PHASE) | new_phase;

    out = atomic_compare_and_set_uint(state, new_state, &gc_state);

  }

  else
    out = true;

  return out;

}


static inline bool gc_thread_try_set_gen(const unsigned int gen) {

  const unsigned char old_gen = gc_thread_next_gen.value;
  const unsigned char new_gen = old_gen > gen ? old_gen : gen;

  return old_gen == new_gen ||
    atomic_compare_and_set_uint(old_gen, new_gen, &gc_thread_next_gen);

}


internal void gc_thread_activate(const unsigned char gen) {

  for(unsigned int i = 1; gc_thread_try_activate(); i++)
    backoff_delay(i);

  for(unsigned int i = 1; gc_thread_try_set_gen(gen); i++)
    backoff_delay(i);

}


static inline void gc_thread_enqueue(gc_closure_t* const restrict closure,
				     volatile void* const obj) {

  closure->gth_queue_size++;

  if(NULL == closure->gth_head) {

    gc_header_list_ptr_set(NULL, (void*)obj);
    closure->gth_head = obj;
    closure->gth_tail = obj;

  }

  else {

    gc_header_list_ptr_set((void*)obj, (void*)(closure->gth_tail));
    gc_header_list_ptr_set(NULL, (void*)obj);
    closure->gth_tail = obj;

  }

}


static inline
volatile void* gc_thread_dequeue(gc_closure_t* const restrict closure) {

  INVARIANT(NULL != closure->gth_head);

  volatile void* const out = closure->gth_head;

  closure->gth_queue_size--;

  if(closure->gth_tail != out)
    closure->gth_head = gc_header_list_ptr(out);

  else {

    closure->gth_tail = NULL;
    closure->gth_head = NULL;

  }

  return out;

}


/* Arrays are handled in a slightly more complex manner, based on
 * exactly what they store.
 *
 * Arrays with less than GC_ARRAY_MIN_COUNT elements, or which occupy
 * less than GC_ARRAY_MIN_SIZE are placed on the local queue and
 * processed by only one thread.  These arrays do not have bitmaps at
 * all.
 */


/* Decide whether the array will be processed by one or multiple threads. */

static inline bool gc_thread_array_shared(const unsigned int num,
					  const unsigned int size) {

  return num * size > GC_ARRAY_MIN_SIZE && num > GC_ARRAY_MIN_COUNT;

}


static inline bool gc_thread_try_push_array(volatile void* const
					    restrict array) {

  void* const top = gc_thread_array_list.value;

  gc_header_list_ptr_set(top, (void*)array);

  return atomic_compare_and_set_ptr(top, (void*)array, &gc_thread_array_list);

}


static inline void gc_thread_push_array(volatile void* const restrict array) {

  for(unsigned int i = 1; !gc_thread_try_push_array(array); i++)
    backoff_delay(i);

}


static inline unsigned int gc_thread_obj_size(const unsigned int nonptr_size,
					      const unsigned int normptrs,
					      const unsigned int weakptrs) {

  return (nonptr_size + ((normptrs + weakptrs) * sizeof(gc_double_ptr_t)));

}


static inline void gc_thread_add_obj(gc_closure_t* const restrict closure,
				     const unsigned int* const type,
				     volatile void* const restrict obj) {

  const gc_typedesc_class_t class = gc_typedesc_class(type);
  const unsigned int nonptr_size = gc_typedesc_nonptr_size(type);
  const unsigned int normptrs = gc_typedesc_normal_ptrs(type);
  const unsigned int weakptrs = gc_typedesc_weak_ptrs(type);
  const unsigned int size = gc_thread_obj_size(nonptr_size, normptrs, weakptrs);

  if(GC_TYPEDESC_NORMAL == class)
    gc_thread_enqueue(closure, obj);

  else {

    const unsigned int num = gc_header_array_len((void*)obj);

    if(!gc_thread_array_shared(num, size))
      gc_thread_enqueue(closure, obj);

    else
      gc_thread_push_array(obj);

  }

}


static inline
unsigned int gc_thread_aligned_normal_size(const unsigned int size) {

  const unsigned int header_size = size + CACHE_LINE_SIZE;
  const unsigned int aligned_size =
    ((header_size - 1) & ~(CACHE_LINE_SIZE - 1)) + CACHE_LINE_SIZE;

  return aligned_size;

}


static inline gen_count_t gc_thread_new_gen_count(const unsigned char oldgen,
						  const unsigned char gen,
						  const unsigned char count) {

  INVARIANT(oldgen == 0xff || oldgen < gc_num_generations);
  INVARIANT(gen == 0xff || gen < gc_num_generations);

  gen_count_t out;

  /* Normal case: either the generation or the count will advance */
  if(gen == oldgen && 0xff != gen || gen != gc_num_generations - 1) {

    /* Advance the count */
    if(count < gc_gen_promote) {

      out.gc_gen = gen;
      out.gc_count = count + 1;

    }

    /* Promote the object */
    else {

      out.gc_gen = gen + 1;
      out.gc_count = 0;

    }

  }

  /* Either the object was just promoted, or it is in the highest
   * generation it will reach, so reset the count.
   */
  else {

    out.gc_gen = gen;
    out.gc_count = 0;

  }

  return out;

}


/* For arrays that are big enough, array elements are broken up into
 * clusters of GC_CLUSTER_SIZE and the work is divided amongst
 * threads.  This is facilitated by the bitmap structure preceeding
 * each array, in which each bit represents a single cluster.  The
 * arrays which have been allocated in the destination space (and are
 * currently being copied) are held in a lock-free simple stack.
 * Threads can attempt to claim clusters for processing when they
 * exhaust their local queues.
 *
 * The simplest case is an array consisting of constant values without
 * any pointers.  These are subject to a basic memcpy of the entire
 * cluster all at once.
 *
 * Arrays which are mutable, but still contain no pointers are copied
 * in the same manner as the non-pointer data of a normal object, but
 * across the entire cluster.
 *
 * Arrays which contain pointers, whether mutable or not are processed
 * entry-by-entry.
 */


/* Get the actual size in bits of an array bitmap for an array with
 * num entries.
 */
static inline unsigned int gc_thread_array_bitmap_bits(const unsigned int num) {

  INVARIANT(num > GC_ARRAY_MIN_COUNT);

  return ((num - 1) / GC_CLUSTER_SIZE) + 1;

}


/* Get the size in bytes of an array bitmap plus the length field,
 * aligned to a cache line.
 */

static inline unsigned int gc_thread_array_bitmap_size(unsigned int num) {

  INVARIANT(num > GC_ARRAY_MIN_COUNT);

  const unsigned int length_bits = sizeof(unsigned int) * 8;
  const unsigned int bits =
    length_bits + gc_thread_array_bitmap_bits(num);
  const unsigned int cache_line_bits = CACHE_LINE_SIZE * 8;
  const unsigned int out =
    ((bits - 1) & ~(cache_line_bits - 1)) + cache_line_bits;

  return out / 8;

}


static inline
unsigned int gc_thread_aligned_array_size(const unsigned int num,
					  const unsigned int size) {

  const unsigned int data_size = size * num;
  const unsigned int header_size = data_size + CACHE_LINE_SIZE;
  const unsigned int bitmap_size = gc_thread_array_shared(num, size) ?
    header_size + gc_thread_array_bitmap_size(num) :
    header_size + CACHE_LINE_SIZE;
  const unsigned int aligned_size =
    ((bitmap_size - 1) & ~(CACHE_LINE_SIZE - 1)) + CACHE_LINE_SIZE;

  return aligned_size;

}


static inline void* gc_thread_array_header(void* const restrict obj,
					   const unsigned int num,
					   const unsigned int size) {

  const unsigned int bitmap_size = gc_thread_array_shared(num, size) ?
    gc_thread_array_bitmap_size(num) : CACHE_LINE_SIZE;
  const unsigned int aligned_size =
    ((bitmap_size - 1) & ~(CACHE_LINE_SIZE - 1)) + CACHE_LINE_SIZE;

  return (char*)obj + aligned_size;

}


static inline volatile void* gc_thread_claim(gc_closure_t* restrict closure,
					     volatile void* restrict obj,
					     unsigned char max_gen,
					     bool copy);


/* Copy one pointer entry over to the destination.  Do not repeatedly
 * copy the pointer.  The check_ptr function does this.
 */

static inline void gc_thread_process_ptr(gc_closure_t* const restrict closure,
					 volatile gc_double_ptr_t srcptr,
					 volatile gc_double_ptr_t dstptr,
					 const unsigned char max_gen,
					 const bool do_weak) {

  const bool flipflop = gc_collection_count % 2;
  const unsigned int used_ptr = flipflop ? 0 : 1;
  const unsigned int unused_ptr = flipflop ? 1 : 0;
  void* const restrict src = srcptr[used_ptr];
  void* const dst = dstptr[unused_ptr];

  /* If not a null pointer, then claim it and assign the result to the
   * field.
   */
  if(NULL != src)
    dstptr[unused_ptr] = (void*)gc_thread_claim(closure, src, max_gen, do_weak);

  /* Otherwise just assign NULL */
  else if(NULL != dst)
    dstptr[unused_ptr] = NULL;

}


static inline void gc_thread_process_weak_ptr(gc_closure_t* const
					      restrict closure,
					      volatile gc_double_ptr_t srcptr,
					      volatile gc_double_ptr_t dstptr,
					      const bool do_weak) {

  const bool flipflop = gc_collection_count % 2;
  const unsigned int used_ptr = flipflop ? 0 : 1;
  const unsigned int unused_ptr = flipflop ? 1 : 0;

  /* If I'm actually processing weak pointers, check to see if it was
   * preserved.  If it was, then copy it.  Otherwise, NULL it out.
   */
  if(do_weak) {

    void* const unclaimed = flipflop ? (void*)0 : (void*)~0;
    void* const restrict src = srcptr[used_ptr];
    void* const dst = dstptr[unused_ptr];
    void* const fwd_ptr = gc_header_fwd_ptr(src);

    /* If the object to which the source points is unclamed, then
     * null out the destination.  Otherwise, copy the pointer.
     */
    if(NULL != src) {

      if(unclaimed != fwd_ptr) {

	if(dst != fwd_ptr)
	  dstptr[unused_ptr] = fwd_ptr;

	}

      else if(NULL != dst)
	dstptr[unused_ptr] = NULL;

    }

    /* Otherwise just assign NULL */
    else if(NULL != dst)
      dstptr[unused_ptr] = NULL;

  }

}


/* Copy a normal object.  The dst and src pointer point to raw data,
 * not to the object headers, allowing this function to be used in
 * array copy functions.
 */
static void gc_thread_copy(gc_closure_t* const restrict closure,
			   volatile void* const restrict dst,
			   volatile void* const restrict src,
			   const unsigned int nonptr_size,
			   const unsigned int num_normptrs,
			   const unsigned int num_weakptrs,
			   const unsigned char max_gen,
			   const bool copied,
			   const bool do_weak) {

  const bool flipflop = gc_collection_count % 2;
  volatile gc_double_ptr_t* const restrict dst_normptrs =
    (volatile gc_double_ptr_t*)((char*)dst + nonptr_size);
  volatile gc_double_ptr_t* const restrict src_normptrs =
    (volatile gc_double_ptr_t*)((char*)src + nonptr_size);
  volatile gc_double_ptr_t* const restrict dst_weakptrs =
    dst_normptrs + num_normptrs;
  volatile gc_double_ptr_t* const restrict src_weakptrs =
    src_normptrs + num_normptrs;

  if(!copied) {

    volatile unsigned char* const restrict dst_nonptr = dst;
    volatile unsigned char* const restrict src_nonptr = src;

    memcpy((void*)dst_nonptr, (void*)src_nonptr, nonptr_size);

    for(unsigned int i = 0; i < num_normptrs; i++)
      gc_thread_process_ptr(closure, src_normptrs[i], dst_normptrs[i],
			    max_gen, do_weak);

  }

  if(do_weak) {

    /* If the object has been copied, and I'm doing weak pointers, I
     * only move over the weak pointers which have objects that
     * survive.
     */
    if(copied)
      for(unsigned int i = 0; i < num_weakptrs; i++)
	gc_thread_process_weak_ptr(closure, src_weakptrs[i],
				   dst_weakptrs[i], do_weak);

    /* Otherwise I move over all the pointers, since the object is new
     * to this phase.
     */
    else
      for(unsigned int i = 0; i < num_weakptrs; i++)
	gc_thread_process_ptr(closure, src_weakptrs[i], dst_weakptrs[i],
			      max_gen, do_weak);

  }

}


/* Arrays get copied differently, depending on whether or not they are
 * purely scalar arrays
 */


/* Copy function for scalar arrays.  This is a straight one-fell-swoop
 * memcpy.
 */
static inline void gc_thread_copy_scalar_array(volatile void* const
					       restrict dst,
					       volatile void* const
					       restrict src,
					       const unsigned int num,
					       const unsigned int size) {

  memcpy((void*)dst, (void*)src, num * size);

}


/* Copy an array or array section.  This does NOT copy the finalizer. */

static inline void gc_thread_copy_array(gc_closure_t* const restrict closure,
					volatile void* const restrict dst,
					volatile void* const restrict src,
					const unsigned int num,
					const unsigned int nonptr_size,
					const unsigned int normptrs,
					const unsigned int weakptrs,
					const unsigned char max_gen,
					const bool copied,
					const bool do_weak) {

  const unsigned int size = gc_thread_obj_size(nonptr_size, normptrs, weakptrs);

  if(0 != normptrs || 0 != weakptrs)
    for(unsigned int i = 0; i < num; i++)
      gc_thread_copy(closure, (char*)dst + (size * i), (char*)src + (size * i),
		     nonptr_size, normptrs, weakptrs, max_gen,
		     copied, do_weak);

  else
    gc_thread_copy_scalar_array(dst, src, num, nonptr_size);

}


/* Check a pointer's value from source to destination, possibly
 * copying it if the destination does not have the correct value
 */
static inline void gc_thread_check_ptr(gc_closure_t* const restrict closure,
				       volatile gc_double_ptr_t dstptr,
				       volatile gc_double_ptr_t srcptr,
				       const unsigned char max_gen,
				       const bool do_weak) {

  const bool flipflop = gc_collection_count % 2;
  const unsigned int used_ptr = flipflop ? 0 : 1;
  const unsigned int unused_ptr = flipflop ? 1 : 0;

  /* Loop necessary to guarantee eventual equivalence. */
  for(;;) {

    void* const restrict src = srcptr[used_ptr];
    void* const dst = dstptr[unused_ptr];

    /* If not a null pointer, then claim it and assign the result to
     * the field
     */
    if(NULL != src) {

      if(dst != gc_header_fwd_ptr(src))
	dstptr[unused_ptr] = (void*)gc_thread_claim(closure, src, max_gen,
						    do_weak);

      else
	break;

    }

    /* Otherwise just assign NULL */
    else if(NULL != dst) {

      dstptr[unused_ptr] = NULL;
      break;

    }

  }

}


static inline void gc_thread_check_weak_ptr(gc_closure_t* const
					    restrict closure,
					    volatile gc_double_ptr_t srcptr,
					    volatile gc_double_ptr_t dstptr) {

  const bool flipflop = gc_collection_count % 2;
  const unsigned int used_ptr = flipflop ? 0 : 1;
  const unsigned int unused_ptr = flipflop ? 1 : 0;

  /* If I'm actually processing weak pointers, check to see if it was
   * preserved.  If it was, then copy it.  Otherwise, NULL it out.
   */
  void* const unclaimed = flipflop ? (void*)0 : (void*)~0;

  for(;;) {

    void* const restrict src = srcptr[used_ptr];
    void* const dst = dstptr[unused_ptr];
    void* const fwd_ptr = gc_header_fwd_ptr(src);

    /* If the object to which the source points is unclamed, then
     * null out the destination.  Otherwise, copy the pointer.
     */
    if(NULL != src) {

      if(unclaimed != fwd_ptr) {

	if(dst != fwd_ptr)
	  dstptr[unused_ptr] = fwd_ptr;

	else
	  break;

      }

      else if(NULL != dst)
	dstptr[unused_ptr] = NULL;

      else
	break;

    }

    /* Otherwise just assign NULL */
    else if(NULL != dst) {

      dstptr[unused_ptr] = NULL;
      break;

    }

  }

}


/* Given a source and destination object, copy from the source to the
 * destination until each field in the destination is observed to be
 * equivalent to the same field in the source.  This is NOT
 * linearizable, see the explanation above
 */
static void gc_thread_check(gc_closure_t* const restrict closure,
			    volatile void* const restrict dst,
			    volatile void* const restrict src,
			    const unsigned int nonptr_size,
			    const unsigned int num_normptrs,
			    const unsigned int num_weakptrs,
			    const unsigned char max_gen,
			    const bool copied,
			    const bool do_weak) {

  volatile unsigned char* const restrict dst_nonptr = dst;
  volatile unsigned char* const restrict src_nonptr = src;
  volatile gc_double_ptr_t* const restrict dst_normptrs =
    (volatile gc_double_ptr_t*)((char*)dst + nonptr_size);
  volatile gc_double_ptr_t* const restrict src_normptrs =
    (volatile gc_double_ptr_t*)((char*)src + nonptr_size);
  volatile gc_double_ptr_t* const restrict dst_weakptrs =
    dst_normptrs + num_normptrs;
  volatile gc_double_ptr_t* const restrict src_weakptrs =
    src_normptrs + num_normptrs;

  if(!copied) {

    /* Compare and possibly copy non-pointers */
    for(unsigned int i = 0; i < nonptr_size; i++)
      while(dst_nonptr[i] != src_nonptr[i])
	dst_nonptr[i] = src_nonptr[i];

    /* Check pointers */
    for(unsigned int i = 0; i < num_normptrs; i++)
      gc_thread_check_ptr(closure, dst_normptrs[i], src_normptrs[i],
			  max_gen, do_weak);

  }

  /* Check weak pointers */
  if(do_weak) {

    /* If the object has been copied, and I'm doing weak pointers, I
     * only move over the weak pointers which have objects that
     * survive.
     */
    if(copied)
      for(unsigned int i = 0; i < num_weakptrs; i++)
	gc_thread_check_weak_ptr(closure, dst_weakptrs[i], src_weakptrs[i]);

    /* Otherwise I move over all the pointers, since the object is new
     * to this phase.
     */
    else
      for(unsigned int i = 0; i < num_weakptrs; i++)
	gc_thread_check_ptr(closure, dst_weakptrs[i], src_weakptrs[i],
			    max_gen, do_weak);

  }

}


/* Arrays also get checked differently, depending on whether or not
 * they are purely scalar arrays
 */


/* Check function for scalar arrays.  This is a straight
 * one-fell-swoop check, reminiscent of the check on non-pointer data
 * in a regular array.
 */
static inline void gc_thread_check_scalar_array(volatile void* const dst,
						volatile void* const src,
						const unsigned int num,
						const unsigned int size) {

  volatile unsigned char* const restrict dst_nonptr = dst;
  volatile unsigned char* const restrict src_nonptr = src;

  /* Compare and possibly copy non-pointers */
  for(unsigned int i = 0; i < num * size; i++)
    while(dst_nonptr[i] != src_nonptr[i])
      dst_nonptr[i] = src_nonptr[i];

}


/* Check an array or array section. */

static inline void gc_thread_check_array(gc_closure_t* const restrict closure,
					 volatile void* const restrict dst,
					 volatile void* const restrict src,
					 const unsigned int num,
					 const unsigned int nonptr_size,
					 const unsigned int normptrs,
					 const unsigned int weakptrs,
					 const unsigned char max_gen,
					 const bool copied,
					 const bool do_weak) {

  const unsigned int size = gc_thread_obj_size(nonptr_size, normptrs, weakptrs);

  if(0 != normptrs || 0 != weakptrs)
    for(unsigned int i = 0; i < num; i++)
      gc_thread_check(closure, (char*)dst + (size * i), (char*)src + (size * i),
		      nonptr_size, normptrs, weakptrs, max_gen,
		      copied, do_weak);

  else
    gc_thread_check_scalar_array(dst, src, num, nonptr_size);

}


/* Given a pointer to an uncollected object's data, claim each pointer
 * and store the new value into the unused pointer slot.
 */
static void gc_thread_update(gc_closure_t* const restrict closure,
			     volatile void* const restrict obj,
			     const unsigned int nonptr_size,
			     const unsigned int num_normptrs,
			     const unsigned int num_weakptrs,
			     const unsigned char max_gen,
			     const bool copied,
			     const bool do_weak) {

  const bool flipflop = gc_collection_count % 2;
  volatile gc_double_ptr_t* const restrict normptrs =
    (volatile gc_double_ptr_t*)((char*)obj + nonptr_size);
  volatile gc_double_ptr_t* const restrict weakptrs = normptrs + num_normptrs;

  /* Claim all normal pointers and assign them to the unused slot */
  if(!copied)
    for(unsigned int i = 0; i < num_normptrs; i++)
      gc_thread_process_ptr(closure, normptrs[i], normptrs[i],
			    max_gen, do_weak);

  /* Process weak pointers */
  if(do_weak) {

    /* If the object has been copied, and I'm doing weak pointers, I
     * only move over the weak pointers which have objects that
     * survive.
     */
    if(copied)
      for(unsigned int i = 0; i < num_weakptrs; i++)
	gc_thread_process_weak_ptr(closure, weakptrs[i], weakptrs[i], do_weak);

    /* Otherwise I move over all the pointers, since the object is new
     * to this phase.
     */
    else
      for(unsigned int i = 0; i < num_weakptrs; i++)
	gc_thread_process_ptr(closure, weakptrs[i], weakptrs[i],
			      max_gen, do_weak);

  }

}


/* Update an array or array section.  This only updates the pointers. */

static void gc_thread_update_array(gc_closure_t* const restrict closure,
				   volatile void* const restrict obj,
				   const unsigned int num,
				   const unsigned int nonptr_size,
				   const unsigned int normptrs,
				   const unsigned int weakptrs,
				   const unsigned char max_gen,
				   const bool copied,
				   const bool do_weak) {

  const unsigned int size = gc_thread_obj_size(nonptr_size, normptrs, weakptrs);

  /* Only need to update pointers */
  if(0 != normptrs || 0 != weakptrs)
    for(unsigned int i = 0; i < num; i++)
      gc_thread_update(closure, (char*)obj + (size * i), nonptr_size,
		       normptrs, weakptrs, max_gen, copied, do_weak);

}


/* Given a pointer to an uncollected object, do a check on each pointer */

static inline void gc_thread_update_check(gc_closure_t* const restrict closure,
					  volatile void* const restrict obj,
					  const unsigned int nonptr_size,
					  const unsigned int num_normptrs,
					  const unsigned int num_weakptrs,
					  const unsigned char max_gen,
					  const bool copied,
					  const bool do_weak) {

  volatile gc_double_ptr_t* const restrict normptrs =
    (volatile gc_double_ptr_t*)((char*)obj + nonptr_size);
  volatile gc_double_ptr_t* const restrict weakptrs =
    normptrs + num_normptrs;

  /* Since this is in-place, there's no need to check the nonpointer
   * data
   */

  if(!copied)
    for(unsigned int i = 0; i < num_normptrs; i++)
      gc_thread_check_ptr(closure, normptrs[i], normptrs[i], max_gen, do_weak);

  if(do_weak) {

    /* If the object has been copied, and I'm doing weak pointers, I
     * only move over the weak pointers which have objects that
     * survive.
     */
    if(copied)
      for(unsigned int i = 0; i < num_weakptrs; i++)
	gc_thread_check_weak_ptr(closure, weakptrs[i], weakptrs[i]);

    /* Otherwise I move over all the pointers, since the object is new
     * to this phase.
     */
    else
      for(unsigned int i = 0; i < num_weakptrs; i++)
	gc_thread_check_ptr(closure, weakptrs[i], weakptrs[i],
			    max_gen, do_weak);

  }

}


static void gc_thread_update_check_array(gc_closure_t* const restrict closure,
					 volatile void* const restrict obj,
					 const unsigned int num,
					 const unsigned int nonptr_size,
					 const unsigned int normptrs,
					 const unsigned int weakptrs,
					 const unsigned char max_gen,
					 const bool copied,
					 const bool do_weak) {

  const unsigned int size = gc_thread_obj_size(nonptr_size, normptrs, weakptrs);

  /* Only need to update pointers */
  if(0 != normptrs)
    for(unsigned int i = 0; i < num; i++)
      gc_thread_update(closure, (char*)obj + (size * i), nonptr_size,
		       normptrs, weakptrs, max_gen, copied, do_weak);

}


static inline bool gc_thread_try_set_copied(volatile void* const restrict obj) {

  void* const restrict old = gc_header_fwd_ptr(obj);
  void* const restrict new = (void*)((unsigned int)old ^ 0x1);

  return gc_header_compare_and_set_fwd_ptr(old, new, obj);

}


static inline void gc_thread_set_copied(volatile void* const restrict obj) {

  for(unsigned int i = 1; !gc_thread_try_set_copied(obj); i++)
    backoff_delay(i);

}


static inline void gc_thread_process(gc_closure_t* const restrict closure,
				     volatile void* const restrict src,
				     const unsigned char max_gen,
				     const bool do_weak) {

  const bool flipflop = gc_collection_count % 2;
  const unsigned int* const type = gc_header_type(src);
  const gc_typedesc_class_t class = gc_typedesc_class(type);
  const unsigned int nonptr_size = gc_typedesc_nonptr_size(type);
  const unsigned int num_normptrs = gc_typedesc_normal_ptrs(type);
  const unsigned int num_weakptrs = gc_typedesc_weak_ptrs(type);
  const unsigned int typeflags = gc_typedesc_flags(type);
  const bool constant = typeflags & GC_TYPEDESC_CONST;
  const unsigned int raw_fwd_ptr = (unsigned int)gc_header_fwd_ptr(src);
  const unsigned int masked_fwd_ptr = raw_fwd_ptr & ~0x3;
  const unsigned int claimed = flipflop ? ~0 : 0;
  const unsigned int masked_claimed = claimed ^ 0x3;

  /* If the object is being collected, copy all its fields.  Objects
   * which are being collected will not have their forwarding pointers
   * set to "claimed".  Note that the lower two bits of the forwarding
   * pointer have to be excluded.
   */
  if(masked_fwd_ptr != masked_claimed) {

    const bool copied = !((unsigned int)raw_fwd_ptr & 0x1);
    void* const restrict dst = (void*)masked_fwd_ptr;
    void* const restrict dst_data = (char*)dst + sizeof(gc_header_t);
    void* const restrict src_data = (char*)src + sizeof(gc_header_t);

    if(GC_TYPEDESC_NORMAL == class) {

      gc_thread_copy(closure, dst_data, src_data, nonptr_size, num_normptrs,
		     num_weakptrs, max_gen, copied, do_weak);

      /* If the object is mutable, do a check */
      if(!constant)
	gc_thread_check(closure, dst_data, src_data, nonptr_size, num_normptrs,
			num_weakptrs, max_gen, copied, do_weak);

    }

    else {

      const unsigned int len = gc_header_array_len(src);

      gc_thread_copy_array(closure, dst_data, src_data, len, nonptr_size,
			   num_normptrs, num_weakptrs, max_gen,
			   copied, do_weak);

      /* If the object is mutable, do a check */
      if(!constant)
	gc_thread_check_array(closure, dst_data, src_data, len, nonptr_size,
			      num_normptrs, num_weakptrs, max_gen,
			      copied, do_weak);

    }

    /* Now mark the object as copied */
    if(!copied)
      gc_thread_set_copied(src);

  }

  /* Otherwise just update the unused side of the double-pointers */
  else {

    /* If it got to this point, the forwarding pointer either is the
     * claimed value, or it is the claimed value with the low bit
     * XORed.
     */
    const bool copied = (raw_fwd_ptr & 0x1) == (claimed & 0x1);
    void* const restrict obj = (char*)src + sizeof(gc_header_t);

    if(GC_TYPEDESC_NORMAL == class) {

      gc_thread_update(closure, obj, nonptr_size, num_normptrs,
		       num_weakptrs, max_gen, copied, do_weak);

      /* If the object is mutable, do a check */
      if(!constant)
	gc_thread_update_check(closure, obj, nonptr_size, num_normptrs,
			       num_weakptrs, max_gen, copied, do_weak);

    }

    else {

      const unsigned int len = gc_header_array_len(obj);

      gc_thread_update_array(closure, obj, len, nonptr_size, num_normptrs,
			     num_weakptrs, max_gen, copied, do_weak);

      /* If the object is mutable, do a check */
      if(!constant)
	gc_thread_update_check_array(closure, obj, len, nonptr_size,
				     num_normptrs, num_weakptrs,
				     max_gen, copied, do_weak);

    }

    /* Now mark the object as copied */
    if(!copied)
      gc_thread_set_copied(obj);

  }

}


/* Process one array cluster */

static inline void gc_thread_process_cluster(gc_closure_t* const
					     restrict closure,
					     volatile void* const restrict src,
					     const unsigned int index,
					     const unsigned char max_gen,
					     const bool do_weak) {

  const bool flipflop = gc_collection_count % 2;
  const unsigned int* const type = gc_header_type(src);
  const unsigned int nonptr_size = gc_typedesc_weak_ptrs(type);
  const unsigned int num_normptrs = gc_typedesc_weak_ptrs(type);
  const unsigned int num_weakptrs = gc_typedesc_weak_ptrs(type);
  const unsigned int typeflags = gc_typedesc_flags(type);
  const bool constant = typeflags & GC_TYPEDESC_CONST;
  const unsigned int real_index = index * GC_CLUSTER_SIZE;
  const unsigned int size =
    gc_thread_obj_size(nonptr_size, num_normptrs, num_weakptrs);
  const unsigned int offset = size * real_index;
  const unsigned int array_len = gc_header_array_len(src);
  const unsigned int available = array_len - real_index;
  const unsigned int num = available > GC_CLUSTER_SIZE ?
    GC_CLUSTER_SIZE : available;
  const unsigned int raw_fwd_ptr = (unsigned int)gc_header_fwd_ptr(src);
  const unsigned int masked_fwd_ptr = raw_fwd_ptr & ~0x3;
  const unsigned int claimed = flipflop ? ~0 : 0;
  const unsigned int masked_claimed = claimed ^ 0x3;

  /* If the object is being collected, copy all its fields.  Objects
   * which are being collected will not have their forwarding pointers
   * set to "claimed".  Note that the lower two bits of the forwarding
   * pointer have to be excluded.
   */
  if(masked_fwd_ptr != masked_claimed) {

    const bool copied = !((unsigned int)raw_fwd_ptr & 0x1);
    void* const restrict dst = (void*)masked_fwd_ptr;
    void* const restrict dst_data = (char*)dst + sizeof(gc_header_t) + offset;
    void* const restrict src_data = (char*)src + sizeof(gc_header_t) + offset;

    gc_thread_copy_array(closure, dst_data, src_data, num, nonptr_size,
			 num_normptrs, num_weakptrs, max_gen,
			 copied, do_weak);

    /* If the object is mutable, do a check */
    if(!constant)
      gc_thread_check_array(closure, dst_data, src_data, num, nonptr_size,
			    num_normptrs, num_weakptrs, max_gen,
			    copied, do_weak);

    /* Now mark the object as copied */
    if(!copied)
      gc_thread_set_copied(src);

  }

  /* Otherwise just update the unused side of the double-pointers */
  else {

    const bool copied = (raw_fwd_ptr & 0x1) == (claimed & 0x1);
    void* const restrict obj_data = (char*)src + sizeof(gc_header_t) + offset;

    gc_thread_update_array(closure, obj_data, num, nonptr_size,
			   num_normptrs, num_weakptrs, max_gen,
			   copied, do_weak);

    /* If the object is mutable, do a check */
    if(!constant)
      gc_thread_update_check_array(closure, obj_data, array_len, nonptr_size,
				   num_normptrs, num_weakptrs, max_gen,
				   copied, do_weak);

    /* Now mark the object as copied */
    if(!copied)
      gc_thread_set_copied(src);

  }

}


/* Returns 1 for success, 0 for try again, -1 for none available. */

static inline int gc_thread_try_claim_cluster(gc_closure_t* const
					      restrict closure,
					      const unsigned char max_gen,
					      const bool do_weak) {

  const bool flipflop = gc_collection_count % 2;
  void* array = gc_thread_array_list.value;
  int out = -1;

  while(out != -1 || NULL != array) {

    INVARIANT(gc_header_array_len(array) > GC_ARRAY_MIN_COUNT);

    const unsigned int num = gc_header_array_len(array);
    const unsigned int bitmap_size = gc_thread_array_bitmap_size(num);
    const unsigned int bitmap_bits = gc_thread_array_bitmap_bits(num);
    void* const restrict bitmap = (char*)array - bitmap_size;
    const int bitmap_index = atomic_bitmap_alloc(bitmap, bitmap_bits, flipflop);

    /* If a legitimate index comes back, then go with it and exit with
     * success.
     */
    if(0 <= bitmap_index) {

      gc_thread_process_cluster(closure, array, bitmap_index, max_gen, do_weak);
      out = 1;

    }

    /* If temporary failure comes back, then propogate it */
    else if(-1 == bitmap_index)
      out = 0;

    /* Otherwise, advance to the next array and try again */
    else {

      void* const new_array = gc_header_list_ptr(array);

      /* Try to CAS in the new array.  If successful, then continue,
       * otherwise fail with a temporary failure.
       */
      if(atomic_compare_and_set_ptr(array, new_array, &gc_thread_array_list))
	array = new_array;

      else
	out = 0;

    }

  }

  return out;

}


/* Claim and process one cluster from shared arrays, return true if
 * one actually got processed, false otherwise.
 */

static inline bool gc_thread_claim_cluster(gc_closure_t* const restrict closure,
					   const unsigned char max_gen,
					   const bool do_weak) {

  int res;

  for(unsigned int i = 1;
      res = gc_thread_try_claim_cluster(closure, max_gen, do_weak);
      i++)
    backoff_delay(i);

  return 0 < res;

}


/* Claim all pointers in the given cluster of global pointers, storing
 * the new pointers to the unused slot in the double-pointer.
 */

static inline void gc_thread_process_globals(gc_closure_t* const
					     restrict closure,
					     const unsigned int index,
					     const unsigned char max_gen,
					     const bool do_weak) {

  const bool flipflop = gc_collection_count % 2;
  const unsigned int used_ptr = flipflop ? 0 : 1;
  const unsigned int unused_ptr = flipflop ? 1 : 0;
  const unsigned int real_index = index * 16;

  for(unsigned int i = real_index;
      i < real_index + 16 && i < gc_global_ptr_count;
      i++) {

    gc_thread_process_ptr(closure, (*gc_global_ptrs)[i], (*gc_global_ptrs)[i],
			  max_gen, do_weak);
    gc_thread_check_ptr(closure, (*gc_global_ptrs)[i], (*gc_global_ptrs)[i],
			max_gen, do_weak);

  }

}

static inline int gc_thread_try_claim_globals(gc_closure_t* const
					      restrict closure,
					      const unsigned char max_gen,
					      const bool do_weak) {

  int out = -1;

  if(0 != gc_global_ptr_count) {

    const bool flipflop = gc_collection_count % 2;
    const unsigned int bitmap_bits =
      gc_thread_array_bitmap_bits(gc_global_ptr_count);
    const int bitmap_index =
      atomic_bitmap_alloc(gc_global_ptr_bitmap, bitmap_bits, flipflop);

    /* If a legitimate index comes back, then go with it and exit with
     * success.
     */
    if(0 <= bitmap_index) {

      gc_thread_process_globals(closure, bitmap_index, max_gen, do_weak);
      out = 1;

    }

    /* If temporary failure comes back, then propogate it */
    else if(-1 == bitmap_index)
      out = 0;

  }

  return out;

}


/* Claim and process one cluster from global pointers, return true if
 * one actually got processed, false otherwise.
 */
static inline bool gc_thread_claim_globals(gc_closure_t* const restrict closure,
					   const unsigned char max_gen,
					   const bool do_weak) {

  int res;

  for(unsigned int i = 1;
      res = gc_thread_try_claim_globals(closure, max_gen, do_weak);
      i++)
    backoff_delay(i);

  return 0 < res;

}


/* Possibly allocate the new object, mark the current object as having
 * been claimed, initialize the object to be collected, then add the
 * object to the queues.
 */
static inline volatile void* gc_thread_claim(gc_closure_t* const
					     restrict closure,
					     volatile void* const restrict obj,
					     const unsigned char max_gen,
					     const bool copy) {

  const bool flipflop = gc_collection_count % 2;
  void* const claimed = flipflop ? (void*)~0 : (void*)0;
  void* const unclaimed = flipflop ? (void*)0 : (void*)~0;
  void* const restrict fwd_ptr = gc_header_fwd_ptr(obj);
  const unsigned int* const restrict type = gc_header_type(obj);
  const gc_typedesc_class_t class = gc_typedesc_class(type);
  const unsigned int nonptr_size = gc_typedesc_nonptr_size(type);
  const unsigned int num_normptrs = gc_typedesc_normal_ptrs(type);
  const unsigned int num_weakptrs = gc_typedesc_weak_ptrs(type);
  const unsigned int len =
    GC_TYPEDESC_NORMAL == class ? 1 : gc_header_array_len(obj);
  const unsigned int flags = gc_header_flags(obj);
  const unsigned int obj_size =
    gc_thread_obj_size(nonptr_size, num_normptrs, num_weakptrs);
  const unsigned int raw_size = obj_size * len;
  const unsigned int aligned_size = GC_TYPEDESC_NORMAL == class ?
    gc_thread_aligned_normal_size(raw_size) :
    gc_thread_aligned_array_size(raw_size, len);
  const unsigned char curr_gen = gc_header_curr_gen(obj);
  const unsigned char next_gen = gc_header_next_gen(obj);
  const unsigned char count = gc_header_count(obj);
  const gen_count_t new_gen_count =
    gc_thread_new_gen_count(curr_gen, next_gen, count);
  volatile void* out;

  /* If the object is being collected, then allocate a copy and CAS
   * the pointer into the forwarding pointer, add the object to the
   * queue if the CAS succeeds 
   */
  if(max_gen >= curr_gen) {

    /* If the object is not claimed, then attempt to claim it */
    if(unclaimed == fwd_ptr) {

      /* Allocate an object.  If it's an array, skip the prelude */
      void* const alloc =
	gc_allocator_gc_prealloc(closure, aligned_size, new_gen_count.gc_gen);
      void* const newobj = GC_TYPEDESC_NORMAL == class ?
	alloc : gc_thread_array_header(alloc, len, obj_size);
      void* const fwd_ptr =
	!copy ? (void*)((unsigned int)newobj ^ 1) : newobj;

      if(gc_header_compare_and_set_fwd_ptr(unclaimed, newobj, obj)) {

	out = newobj;
	gc_allocator_gc_postalloc(closure, aligned_size, new_gen_count.gc_gen);

	/* Initialize the header.  The forwarding pointer gets
	 * initialized to claimed, because these are reversed at the
	 * end of collection.
	 */
	if(GC_TYPEDESC_NORMAL == class)
	  gc_header_init_normal(claimed, type, flags,
				new_gen_count.gc_gen,
				next_gen,
				new_gen_count.gc_count,
				newobj);

	else {

	  gc_header_init_array(claimed, type, flags,
			       new_gen_count.gc_gen,
			       next_gen,
			       new_gen_count.gc_count,
			       len, newobj);

	  /* Initialize the bitmap if it exists */
	  if(gc_thread_array_shared(len, obj_size)) {

	    const unsigned char bitmap_value = flipflop ? 0xff : 0;
	    const unsigned int bitmap_size =
	      gc_thread_array_bitmap_size(len) - sizeof(unsigned int);

	    memset(alloc, bitmap_value, bitmap_size);

	  }

	}

	gc_thread_add_obj(closure, type, obj);

      }

      else
	out = gc_header_fwd_ptr(obj);

    }

    /* Otherwise, return the forwarding pointer's value */
    else
      out = fwd_ptr;

  }

  /* Otherwise CAS the claimed tag into the forwarding pointer, and
   * add the object to the queue if the CAS succeeds.
   */
  else {

    void* const fwd_ptr =
      !copy ? (void*)((unsigned int)claimed ^ 1) : claimed;

    out = obj;

    if(unclaimed == fwd_ptr)
      if(gc_header_compare_and_set_fwd_ptr(unclaimed, claimed, obj))
	gc_thread_add_obj(closure, type, obj);

  }

  return out;

}



/* Process a single write log entry */

static inline void gc_thread_process_write_entry(gc_closure_t* const
						 restrict closure,
						 void* const restrict ent,
						 const unsigned char max_gen,
						 const bool do_weak) {

  void* const src = gc_log_entry_objptr(ent);
  const bool flipflop = gc_collection_count % 2;
  void* const unclaimed = flipflop ? (void*)0 : (void*)~0L;
  volatile void* const dst = gc_header_fwd_ptr(src);

  /* Unclaimed objects are simply ignored. */
  if(unclaimed != dst) {

    const unsigned char curr_gen = gc_header_curr_gen(src);
    const unsigned int offset = gc_log_entry_offset(ent);
    const unsigned int real_offset = sizeof(gc_header_t) + offset;
    const unsigned int* const type = gc_header_type((void*)src);
    const unsigned int nonptr_size = gc_typedesc_nonptr_size(type);
    const unsigned int num_normptrs = gc_typedesc_normal_ptrs(type);
    const unsigned int num_weakptrs = gc_typedesc_weak_ptrs(type);
    const unsigned int normptr_end =
      nonptr_size + (num_normptrs * sizeof(gc_double_ptr_t));

    /* If the object is being collected, copy the field entirely */
    if(max_gen >= curr_gen) {

      /* If the offset falls within non-pointer space, simply copy
       * from src to new.
       */
      if(offset < nonptr_size) {

	unsigned int* const src_ptr = (unsigned int*)((char*)src + real_offset);
	unsigned int* const dst_ptr = (unsigned int*)((char*)dst + real_offset);

	while(*dst_ptr != *src_ptr)
	  *dst_ptr = *src_ptr;

      }

      /* Otherwise, do the check pointer procedure, always if it's a
       * normal pointer, and weak only if this is the second pass.
       */
      else {

	volatile void* const srcptr =
	  (volatile void*)((char*)src + real_offset);
	volatile void* const dstptr =
	  (volatile void*)((char*)dst + real_offset);

	if(offset < normptr_end)
	  gc_thread_check_ptr(closure, srcptr, dstptr, max_gen, do_weak);

	else if(do_weak)
	  gc_thread_check_ptr(closure, srcptr, dstptr, max_gen, do_weak);

      }

    }

    /* Otherwise only copy it if it's a pointer */
    else if(offset > nonptr_size) {

	volatile void* const srcptr =
	  (volatile void*)((char*)src + real_offset);
	volatile void* const dstptr =
	  (volatile void*)((char*)dst + real_offset);

      /* Do the check pointer procedure, always if it's a
       * normal pointer, and weak only if this is the second pass.
       */
      if(offset < normptr_end)
	gc_thread_check_ptr(closure, srcptr, dstptr, max_gen, do_weak);

      else if(do_weak)
	gc_thread_check_ptr(closure, srcptr, dstptr, max_gen, do_weak);

    }

  }

}


static inline
unsigned int gc_thread_hash_table_index(gc_write_log_hash_node_t* const
					restrict node) {

  const unsigned int hash =
    (unsigned int)(gc_log_entry_objptr(node->wh_log_entry));

  return hash % 4091;

}


/* Insert only if there is not already something in the table */

static inline void gc_thread_hash_table_insert(gc_closure_t* const
					       restrict closure,
					       gc_write_log_hash_node_t* const
					       restrict node) {

  void* const addr = gc_log_entry_objptr(node->wh_log_entry);
  const unsigned int index = gc_thread_hash_table_index(node);
  gc_write_log_hash_node_t* curr = thread->gth_hash_table[index];
  bool present = false;

  /* Check to see if it's there */
  while(NULL != curr)
    if(addr == gc_log_entry_objptr(curr->wh_log_entry)) {

      present = true;
      break;

    }

    else
      curr = curr->wh_hash_next;

  /* If not, add it to the table and to the list */
  if(!present) {

    node->wh_hash_next = curr;
    closure->gth_hash_table[index] = node;
    node->wh_list_next = closure->gth_unique_list;
    closure->gth_unique_list = node;

  }

}


/* This function makes use of a per-thread hash table and nodes to
 * avoid processing a given location multiple times.
 */

static inline void gc_thread_clear_write_log(gc_closure_t* const
					     restrict closure,
					     const unsigned int index,
					     const unsigned char max_gen,
					     const bool do_weak) {

  gc_write_log_hash_node_t* curr;

  /* There is no need to clear the hash table (which would take a long
   * time), as entries will be wiped out at the end, when the table is
   * scanned.
   */
  for(unsigned int i = 0; i < index; i++)
    /* The hash node at index i is initialized to point to the write
     * log entry at index i, and this never changes.
     */
    gc_thread_hash_table_insert(closure, closure->gth_hash_nodes + i);

  /* The hash table aspect is only used to spot duplicates.  At this
   * point, the unique list is the only thing that matters.
   */
  curr = closure->gth_unique_list;

  while(NULL != curr) {

    /* Calculate the index from the address, and clear that slot in
     * the hash table.
     */
    const unsigned int index = gc_thread_hash_table_index(curr);

    closure->gth_hash_table[index] = NULL;
    gc_thread_process_write_entry(closure, curr->wh_log_entry, max_gen, do_weak);
    curr = curr->wh_list_next;

  }

  closure->gth_unique_list = NULL;

}


/* Compute the generation for the collection as a function of the
 * previous generation and the requested next generation.
 */
static inline unsigned int gc_thread_get_gen(void) {

  const unsigned int current_gen = gc_thread_next_gen.value;

  INVARIANT(current_gen >= gc_num_generations);
  INVARIANT(gc_thread_peak_gen >= gc_num_generations);

  /* If the current generation is less than the peak, business as usual */
  if(current_gen < gc_thread_peak_gen)
    gc_thread_next_gen.value++;

  /* If the current generation is equal to the peak, advance the peak
   * and return to 1.
   */
  else if(current_gen == gc_thread_peak_gen) {

    gc_thread_peak_gen = current_gen < gc_num_generations ?
      gc_thread_peak_gen + 1 : 1;
    gc_thread_next_gen.value = 1;

  }

  /* Otherwise the requested generation has been artificially set
   * higher than the peak was.  Pretend that I just hit a peak at this
   * generation.
   */
  else
    gc_thread_peak_gen = current_gen < gc_num_generations ?
      current_gen + 1 : 1;


  return current_gen;

}


/* Attempt the start barrier.  All collector threads will enter and
 * halt at this barrier until the collector has been turned on.  Once
 * the collector is turned on, this will act as a normal barrier
 * requiring all executors to acknowledge that the collector is now
 * on.
 */
static void gc_thread_initial_barrier(void) {

  if(os_thread_barrier_enter(&gc_thread_initial_barrier_value)) {

    /* Start barrier sequential code: Turn the collector on. */
    const unsigned int old_state = gc_state.value;
    const unsigned int gen = gc_thread_get_gen();
    const unsigned int new_state =
      (old_state & ~(GC_STATE_PHASE | GC_STATE_GEN)) | GC_STATE_NORMAL | gen;

    gc_state.value = new_state;
    os_thread_barrier_release(&gc_thread_initial_barrier_value);

  }

}


static inline void gc_thread_middle_barrier(void) {

  if(os_thread_barrier_enter(&gc_thread_middle_barrier_value)) {

    /* Middle barrier sequential code: switch to weak pointer
     * preservation mode.
     */
    const unsigned int old_state = gc_state.value;
    const unsigned int new_state =
      (old_state & ~GC_STATE_PHASE) | GC_STATE_WEAK;

    gc_state.value = new_state;
    os_thread_barrier_release(&gc_thread_middle_barrier_value);

  }

}


/* Attempt the final barrier, possibly having to abort and clear more
 * derived values.  If this succeeds, the collector will be disabled,
 * and execution status will return to normal.  Again, the entire root
 * set must have been touched prior to calling this, and a
 * GC_STATE_CHANGE signal must have been sent via executor mailboxes.
 */
static inline void gc_thread_final_barrier(gc_closure_t* const restrict closure,
					   volatile gc_allocator_t*
					   const allocators) {

  if(os_thread_barrier_enter(&gc_thread_final_barrier_value)) {

    /* Middle barrier sequential code: turn off the collector and free
     * all the memory.
     */
    const unsigned int old_state = gc_state.value;
    const unsigned int gen = old_state & GC_STATE_GEN;
    const unsigned int new_state =
      (old_state & ~(GC_STATE_PHASE | GC_STATE_GEN)) | GC_STATE_INACTIVE;

    gc_thread_peak_gen = gen;
    gc_collection_count++;
    gc_state.value = new_state;
    gc_allocator_release_slices(gen);
    os_thread_barrier_release(&gc_thread_final_barrier_value);

  }
 
  /* Allocator 0 is uncollected space, so skip it, but copy all the
   * other allocators over to the executor's space.
   */
  memcpy(allocators + 1, closure->gth_allocators + 1,
	 gc_num_generations * sizeof(gc_allocator_t));

}


/* Remember for both of these functions that cc_safepoint expects that
 * state will have already been saved into the mailbox, so by simply
 * failing to save it, we correctly do the safepoint_without_save
 * functionality.
 */
static inline void gc_thread_yield(gc_closure_t* const restrict closure,
				   volatile gc_allocator_t* const allocators,
				   const unsigned int exec) {

  /* Allocator 0 is uncollected space, and is used by both the
   * collector and the program, so it needs to be transferred into and
   * out of the gth_allocators field.
   */
  memcpy(allocators, closure->gth_allocators, sizeof(gc_allocator_t));
  cc_safepoint(exec, EX_SIGNAL_SCHEDULE);

}


static inline void gc_safepoint(gc_closure_t* const restrict closure,
				volatile gc_allocator_t* const allocators,
				const unsigned int exec) {

  /* Allocator 0 is uncollected space, and is used by both the
   * collector and the program, so it needs to be transferred into and
   * out of the gth_allocators field.
   */
  memcpy(allocators, closure->gth_allocators, sizeof(gc_allocator_t));
  cc_safepoint(exec, 0);

}


static inline bool gc_thread_push(gc_closure_t* const restrict closure,
				  const unsigned int exec) {

  void* const obj = gc_thread_dequeue(closure);
  bool out;

  if(out = lf_object_queue_enqueue(gc_workshare, obj, exec))
    PRINTD("Executor %d successfully enqueued\n", exec);

  else {

    PRINTD("Executor %d failed to enqueue\n", exec);
    gc_thread_enqueue(closure, obj);

  }

  return out;

}


static inline bool gc_thread_pull(gc_closure_t* const restrict closure,
				  const unsigned int exec) {

  void* const obj = lf_object_queue_dequeue(gc_workshare, exec);
  bool out;

  if(bool = (NULL != obj)) {

    PRINTD("Executor %d successfully dequeued\n", exec);
    gc_thread_enqueue(closure, obj);

  }

  else
    PRINTD("Executor %d failed to dequeue\n", exec);

  return out;

}


static inline void gc_thread_balance(gc_closure_t* const restrict closure,
				     const unsigned int exec) {

  const unsigned int local = closure->gth_queue_size;
  const int count = closure->gth_workshare_count;
  const int intent = count + (moderator - local);

  PRINTD("Executor %u balancing.  Count is %d, size is %u\n",
	 exec, count, local);

  if(0 < intent) {

    PRINTD("Executor %u intends to push\n", exec);

    if(0 < count) {

      if(gc_thread_push(closure, exec)) {

	PRINTD("Executor %u push succeeded\n", exec);
	closure->gth_workshare_count--;

      }

      else {

	PRINTD("Executor %u push failed\n", exec);
	closure->gth_workshare_count = -moderator;

      }

    }

    else {

      PRINTD("Executor %u deferring push\n", exec);
      closure->gth_workshare_count--;

    }

  }

  else {

    PRINTD("Executor %u intends to pull\n", exec);

    if(0 > count) {

      if(gc_thread_push(closure, exec)) {

	PRINTD("Executor %u pull succeeded\n", exec);
	closure->gth_workshare_count++;

      }

      else {

	PRINTD("Executor %u pull failed\n", exec);
	closure->gth_workshare_count = moderator;

      }

    }

    else {

      PRINTD("Executor %u deferring pull\n", exec);
      closure->gth_workshare_count++;

    }

  }

}


/* This is the top-level function for a garbage collector thread.
 * This represents one attempt at completing a GC-cycle.  The state of
 * garbage collector threads is not preserved in a context-switch, so
 */
static void gc_thread(gc_closure_t* const restrict closure,
		      volatile gc_log_entry_t* const write_log,
		      volatile thread_mbox_t mbox,
		      const unsigned int exec) {

  /* XXX Probably want to make a fast path, which just clears the
   * write log and executes a barrier.
   */

  volatile gc_allocator_t* volatile* const allocator_ptr =
    thread_mbox_allocators(mbox);
  volatile gc_allocator_t* const allocators = *allocator_ptr;
  unsigned int state = gc_state.value;

  /* Allocator 0 is uncollected space, and is used by both the
   * collector and the program, so it needs to be transferred into and
   * out of the gth_allocators field.
   */
  memcpy(closure->gth_allocators, allocators, sizeof(gc_allocator_t));
  state = gc_state.value;
  INVARIANT(state & GC_STATE_PHASE != GC_STATE_INACTIVE);

  /* Clear the log if I'm not in the initial state (if I am, I have no
   * log)
   */
  if(GC_STATE_INITIAL != state & GC_STATE_PHASE) {

    volatile unsigned int* const write_log_index_ptr =
      thread_mbox_write_log_index(mbox);
    const unsigned int write_log_index = *write_log_index_ptr;


    /* Remember, gc_thread_last_gen will hold the current generation
     * at this point, if I get here.
     */
    gc_thread_clear_write_log(closure, gc_thread_last_gen, write_log_index,
			      GC_STATE_WEAK == state);
    *write_log_index_ptr = 0;

  }

  /* The thread function loops infinitely in case a thread passes the
   * final barrier, but someone else starts another collection before
   * it manages to yield.
   */
  for(;;) {

    state = gc_state.value;
    INVARIANT(state & GC_STATE_PHASE != GC_STATE_INACTIVE);

    /* If I've passed the initial barrier, then clear my write log,
     * otherwise go for the barrier.
     */
    if(GC_STATE_INITIAL == state & GC_STATE_PHASE)
      gc_thread_initial_barrier();

    /* Once the initial barrier is passed, gc_thread_last_gen holds
     * the current generation, paradoxically enough
     */

    state = gc_state.value;
    INVARIANT(state & GC_STATE_PHASE != GC_STATE_INACTIVE);

    /* In the normal state, try to copy the memory graph */
    if(GC_STATE_NORMAL == state & GC_STATE_PHASE) {

      /* XXX need to scan all threads as well */
      for(unsigned int i = 1;
	  gc_thread_claim_cluster(closure, gc_thread_last_gen, false) ||
	    gc_thread_claim_globals(closure, gc_thread_last_gen, false);)
	while(NULL != closure->gth_head) {

	  volatile void* const obj = gc_thread_dequeue(closure);

	  gc_thread_process(closure, obj, gc_thread_last_gen, false);
	  i = (i + 1) % 16;

	  if(0 == i) {

	    gc_thread_balance(closure);
	    gc_safepoint(closure, allocators, exec);

	  }

	}

      gc_thread_middle_barrier();

    }

    /* Now get all the weak pointers.  I can only be in the weak state
     * at this point.
     */
    INVARIANT(gc_state.value & GC_STATE_PHASE == GC_STATE_WEAK);

    for(unsigned int i = 1;
	gc_thread_claim_cluster(closure, gc_thread_last_gen, true) ||
	  gc_thread_claim_globals(closure, gc_thread_last_gen, true);)
      while(NULL != closure->gth_head) {

	volatile void* const obj = gc_thread_dequeue(closure);

	gc_thread_process(closure, obj, gc_thread_last_gen, true);
	i = (i + 1) % 16;

	if(0 == i) {

	  gc_thread_balance(closure);
	  gc_safepoint(closure, allocators, exec);

	}

      }

    gc_thread_final_barrier(closure, allocators);

    /* Yield the processor if another GC cycle beginning hasn't intervened */
    if(GC_STATE_INACTIVE == gc_state.value) {

      gc_thread_yield(closure, allocators, exec);
      panic("Executor %u reached invalid point in GC thread!\n", exec);

    }

  }

}
