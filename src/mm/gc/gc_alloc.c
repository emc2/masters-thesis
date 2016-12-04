/* Copyright (c) 2008 Eric McCorkle.  All rights reserved */

#include <stdio.h>
#include <stdlib.h>
#include "definitions.h"
#include "panic.h"
#include "atomic.h"
#include "bitops.h"
#include "gc.h"
#include "mm/gc_alloc.h"
#include "mm/gc_thread.h"
#include "mm/gc_vars.h"
#include "mm/slice.h"


#define MIN_SLICE_POWER 14
#define MAX_SLICE_POWER BITS - 1
#define SLICE_POWERS (((MAX_SLICE_POWER) - (MIN_SLICE_POWER)) + 1)
#define USAGE_RATIO 2

/*!
 * This is the hard limit of total space to used space.  Going below
 * this ratio will cause all requests to allocate new slices, and
 * abort the request if slices cannot be allocated.
 *
 * \brief The hard ratio of total space to used space.
 */
static float gc_hard_ratio;

/*!
 * This is the soft limit of total space to used space.  Going below
 * this will start a collection.
 *
 * \brief The soft ratio of total space to used space.
 */
static float gc_soft_ratio;

/* These could be fifos, except there would be quite a few fifos, and
 * the preallocated nodes would take up hundreds of megabytes.  Having
 * a separate queue for each slice type for each executor would cause
 * a similar problem.  Since they are seldom accessed, these are
 * stored as lock-free simple stacks.
 *
 * These are actually arrays of atomic pointer, indexed as follows:
 * gc_used_slices[slice power][generation].
 */
static volatile atomic_ptr_t* gc_used_slices[SLICE_POWERS];
static volatile atomic_ptr_t* gc_free_slices[SLICE_POWERS];
static volatile atomic_ptr_t* gc_new_slices[SLICE_POWERS];
static volatile atomic_uint_t* gc_used_space;
static volatile atomic_uint_t* gc_free_space;
static volatile atomic_uint_t* gc_new_space;


static slice_t* gc_allocator_alloc_slice(volatile atomic_ptr_t*
					 const restrict src,
					 volatile atomic_ptr_t*
					 const restrict dst) {

  slice_t* out;

  /* Try to pull a slice off of the source. */
  for(unsigned int i = 1;; i++) {

    out = src->value;

    if(NULL == out || atomic_compare_and_set_ptr(out, out->s_next, src))
      break;

    else
      backoff_delay(i);

  }

  /* At this point, we've succeeded or failed.  Since the queues are
   * switched inside a barrier action, it is ok for this to be
   * non-linearizable.  If I got one, put it on the destination.
   */
  if(NULL != out)
    for(unsigned int i = 1;; i++) {

      slice_t* const dst_value = dst->value;

      out->s_next = dst_value;

      if(atomic_compare_and_set_ptr(dst_value, out, dst))
	break;

      else
	backoff_delay(i);

    }

  return out;

}


/* These are somewhat robust, but not linearizable snapshots */

static unsigned int gc_allocator_total_used_space(void) {

  unsigned int snapshot[gc_num_generations];
  unsigned int sum = 0;
  bool valid = false;

  /* Take the snapshot */
  for(unsigned int i = 1; !valid; i++) {

    valid = true;

    for(unsigned int j = 0; j < gc_num_generations; j++)
      snapshot[j] = gc_used_space[j].value;

    for(unsigned int j = 0; j < gc_num_generations; j++)
      valid &= snapshot[j] == gc_used_space[j].value;

    if(!valid)
      backoff_delay(i);

  }

  /* Sum the values */
  for(unsigned int i = 0; i < gc_num_generations; i++)
    sum += snapshot[i];

  return sum;

}


static unsigned int gc_allocator_total_new_space(void) {

  unsigned int snapshot[gc_num_generations];
  unsigned int sum = 0;
  bool valid = false;

  /* Take the snapshot */
  for(unsigned int i = 1; !valid; i++) {

    valid = true;

    for(unsigned int j = 0; j < gc_num_generations; j++)
      snapshot[j] = gc_new_space[j].value;

    for(unsigned int j = 0; j < gc_num_generations; j++)
      valid &= snapshot[j] == gc_new_space[j].value;

    if(!valid)
      backoff_delay(i);

  }

  /* Sum the values */
  for(unsigned int i = 0; i < gc_num_generations; i++)
    sum += snapshot[i];

  return sum;

}


static unsigned int gc_allocator_total_free_space(void) {

  unsigned int snapshot[gc_num_generations];
  unsigned int sum = 0;
  bool valid = false;

  /* Take the snapshot */
  for(unsigned int i = 1; !valid; i++) {

    valid = true;

    for(unsigned int j = 0; j < gc_num_generations; j++)
      snapshot[j] = gc_free_space[j].value;

    for(unsigned int j = 0; j < gc_num_generations; j++)
      valid &= snapshot[j] == gc_free_space[j].value;

    if(!valid)
      backoff_delay(i);

  }

  /* Sum the values */
  for(unsigned int i = 0; i < gc_num_generations; i++)
    sum += snapshot[i];

  return sum;

}


/* It is ok for this to be not-quite-linearizable.  Checks to see if
 * the hard limit is exceeded by a request.
 */
static bool gc_allocator_limit_check(const unsigned int req_size) {

  const unsigned int used_size = gc_allocator_total_used_space();
  const unsigned int new_size = gc_allocator_total_new_space();
  const unsigned int free_size = gc_allocator_total_free_space();
  const unsigned int total_size = used_size + new_size + free_size;
  const unsigned int total_used_size = used_size + req_size;
  const float ratio = (1.0 * total_size) / total_used_size;

  return ratio >= gc_hard_ratio;

}


/* Check the soft ratio and maybe activate the collector.  Again, this
 * is ok to be not-quite linearizable.
 */
static void gc_allocator_check_activate_collector(void) {

  const unsigned int used_size = gc_allocator_total_used_space();
  const unsigned int new_size = gc_allocator_total_new_space();
  const unsigned int free_size = gc_allocator_total_free_space();
  const unsigned int total_size = used_size + new_size + free_size;
  const float ratio = (1.0 * total_size) / used_size;

  if(GC_STATE_INACTIVE == gc_state.value & GC_STATE_PHASE &&
     ratio >= gc_soft_ratio)
    /* I don't need to CAS, since this will only change to something
     * other than initial when everyone (including me) passes the
     * barrier.
     */
    gc_state.value = GC_STATE_INITIAL;

}


/* This does need to be linearizable. */
static void gc_allocator_update_sizes(const unsigned int req_size,
				      const unsigned int gen,
				      const bool new,
				      const bool for_gc) {

  if(!for_gc) {

    for(unsigned int i = 1;; i++) {

      const unsigned int used_size = gc_used_space[gen - 1].value;

      if(atomic_compare_and_set_uint(used_size, used_size + req_size,
				     &gc_used_space))
	break;

      else
	backoff_delay(i);

    }

  }

  else
    for(unsigned int i = 1;; i++) {

      const unsigned int new_size = gc_new_space[gen - 1].value;

      if(atomic_compare_and_set_uint(new_size, new_size + req_size,
				     &gc_new_space))
	break;

      else
	backoff_delay(i);

    }

  /* If not a new slice, then decrement free_size */
  if(!new) {

    for(unsigned int i = 1;; i++) {

      const unsigned int free_size = gc_free_space[gen - 1].value;

      if(atomic_compare_and_set_uint(free_size, free_size - req_size,
				     &gc_free_space))
	break;

      else
	backoff_delay(i);

    }

  }

  /* Second part: maybe switch on the collector */
  if(!for_gc)
    gc_allocator_check_activate_collector();

}


static bool gc_allocator_do_refresh(gc_allocator_t allocator,
				    const unsigned int min,
				    const unsigned int target,
				    const unsigned int gen,
				    const bool for_gc) {

  INVARIANT(gen != 0);
  INVARIANT(min <= target);

  const unsigned int min_raw_power = bitscan_high(min);
  const unsigned int target_raw_power = bitscan_high(target);
  const unsigned int index = gen - 1;
  bool out = false;

  INVARIANT(min_raw_power <= MAX_SLICE_POWER);
  INVARIANT(target_raw_power <= MAX_SLICE_POWER);

  const unsigned int min_slice_power = min_raw_power > MIN_SLICE_POWER ?
    min_raw_power - MIN_SLICE_POWER : 0;
  const unsigned int target_slice_power = target_raw_power > MIN_SLICE_POWER ?
    target_raw_power - MIN_SLICE_POWER : 0;
  slice_t* slice = NULL;

  /* First try the target, and count down to the minimum. */
  for(unsigned int i = target_slice_power;
      i >= min_slice_power && NULL == slice; i--)
    /* Check to make sure the size isn't too much, ignoring it if this
     * is for GC.
     */
    if(for_gc || gc_allocator_limit_check(0x1 << (i + MIN_SLICE_POWER)))
    /* If doing it for garbage collection, add to the new slices,
     * otherwise, add to the used slices.
     */
      if(NULL !=
	 (slice = gc_allocator_alloc_slice(gc_free_slices[i] + index,
					   !for_gc ? gc_used_slices[i] + index :
					   gc_new_slices[i] + index)))
	gc_allocator_update_sizes(0x1 << (i + MIN_SLICE_POWER),
				  gen, false, for_gc);

  /* XXX This is going to be very inefficient, and will not
   * keep memory usage down.  Figure out how to intelligently release
   * slices and redo this part.
   */

  /* If this is for GC, ignore the limit, otherwise, obey it */
  for(unsigned int i = min_slice_power + 1;
      i < SLICE_POWERS && NULL == slice &&
	(for_gc || gc_allocator_limit_check(0x1 << (i + MIN_SLICE_POWER)));
      i++)
    /* If doing it for garbage collection, add to the new slices,
     * otherwise, add to the used slices.
     */
    if(NULL !=
       (slice = gc_allocator_alloc_slice(gc_free_slices[i] + index,
					 !for_gc ? gc_used_slices[i] + index :
					 gc_new_slices[i] + index)))
      gc_allocator_update_sizes(0x1 << (i + MIN_SLICE_POWER),
				gen, false, for_gc);

  /* If no slice has been found, try to allocate one. */
  for(unsigned int i = target_slice_power;
      i >= min_slice_power && NULL == slice; i--)
    if(NULL != (slice = slice_alloc(SLICE_TYPE_GC, SLICE_PROT_RWX,
				    0x1 << (i + MIN_SLICE_POWER)))) {

      /* If allocation succeeds, insert it into the right stack */
      volatile atomic_ptr_t* const restrict dst =
	for_gc ? gc_used_slices[i] + index : gc_new_slices[i] + index;

      /* Add it to the destination */
      for(unsigned int i = 1;; i++) {

	slice_t* const dst_value = dst->value;

	slice->s_next = dst_value;

	if(atomic_compare_and_set_ptr(dst_value, slice, dst))
	  break;

	else
	  backoff_delay(i);

      }

      gc_allocator_update_sizes(0x1 << (i + MIN_SLICE_POWER),
				gen, true, for_gc);

    }

  /* If successful update the allocator accordingly */
  if(NULL != slice) {

    out = true;
    allocator[0] = slice->s_ptr;
    allocator[1] = (char*)(slice->s_ptr) + (slice->s_size);

  }

  return out;

}


bool gc_allocator_refresh(gc_allocator_t allocator,
			  const unsigned int min,
			  const unsigned int target,
			  const unsigned int gen) {

  /* XXX update the documentation.  This function cannot deal with
   * failures internally, because it cannot suspend the caller.
   */
  return gc_allocator_do_refresh(allocator, min, target, gen, false);

}


static inline unsigned int get_target_size(const unsigned int min) {

  static const unsigned int max_size = 0x1 << MAX_SLICE_POWER;
  static const unsigned int min_size = 0x1 << MIN_SLICE_POWER;
  const unsigned int default_size = min * 8;
  unsigned int out;

  if(max_size / 8 > min) {

    if(min_size < default_size)
      out = default_size;

    else
      out = min_size;

  }

  else
    out = max_size;

  return out;

}


internal void* gc_allocator_alloc(gc_allocator_t allocator,
				  const unsigned int size,
				  const unsigned int gen) {

  const unsigned int target = get_target_size(size);
  char* const newptr = (char*)(allocator[0]) + size;
  void* out = NULL;

  /* If there is enough memory there already, just allocate it. */
  if(newptr <= (char*)allocator[1]) {

    out = allocator[0];
    allocator[0] = newptr;

  }

  /* Otherwise try to get more. */
  else if(gc_allocator_refresh(allocator, size, target, gen)) {

    /* If there still isn't enough, something went wrong */
    if(newptr <= (char*)allocator[1]) {

      out = allocator[0];
      allocator[0] = newptr;

    }

    else
      panic("Error: gc_allocator_refresh didn't allocate enough space.\n");

  }

  return out;

}


internal void* gc_allocator_gc_prealloc(gc_closure_t* const restrict closure,
					const unsigned int size,
					const unsigned int gen) {

  const unsigned int target = get_target_size(size);
  char* const newptr = (char*)(closure->gth_allocators[gen - 1][0]) + size;
  void* out = NULL;

  /* This function's structure mirrors gc_alloc */

  /* If there is enough memory there already, just allocate it. */
  if(newptr <= (char*)(closure->gth_allocators[gen - 1][1]))
    out = closure->gth_allocators[gen - 1][0];

  /* Otherwise try to get more. */
  else if(gc_allocator_do_refresh(closure->gth_allocators[gen - 1],
				  size, target, gen, true)) {

    /* If there still isn't enough, something went wrong */
    if(newptr <= (char*)(closure->gth_allocators[gen - 1][1]))
      out = closure->gth_allocators[gen - 1][0];

    else
      panic("Error: gc_allocator_refresh didn't allocate enough space.\n");

  }

  return out;

}


internal void gc_allocator_gc_postalloc(gc_closure_t* const restrict closure,
					const unsigned int size,
					const unsigned int gen) {

  char* const newptr = (char*)(closure->gth_allocators[gen - 1][0]) + size;

  /* If there isn't enough space, something went wrong */
  if(newptr <= (char*)(closure->gth_allocators[gen - 1][1]))
    closure->gth_allocators[gen - 1][0] = newptr;

  else
    panic("Error: not enough space in call to gc_allocator_gc_postalloc.\n");

}


/* This function is not lock-free.  It is called only by the last
 * thread to pass the final barrier.  Therefore, it is safe to assume
 * that it is executed only by one thread.
 */
internal void gc_allocator_release_slices(const unsigned int gen) {

  /* Append the used space to the free space (free old heap). */
  for(unsigned int i = 0; i < SLICE_POWERS; i++)
    for(unsigned int j = 0; j < gen - 1; j++) {

      slice_t* curr = gc_free_slices[i][j].value;

      /* If there are no free slices, then the used slices are just copied */
      if(NULL == curr)
	gc_free_slices[i][j].value = gc_used_slices[i][j].value;

      /* Otherwise append the used slices */
      else {

	while(NULL != curr->s_next)
	  curr = curr->s_next;

	curr->s_next = gc_used_slices[i][j].value;

      }

    }

  /* Add the current used space counters to the free space counters */
  for(unsigned int i = 0; i < gen - 1; i++)
    gc_free_space[i].value += gc_used_space[i].value;

  /* The new space becomes the used space (transition to new heap image). */
  for(unsigned int i = 0; i < SLICE_POWERS; i++)
    for(unsigned int j = 0; j < gen - 1; j++)
      gc_used_slices[i][j].value = gc_new_slices[i][j].value;

  /* Set the used space counters to the current new space counters and
   * zero out the new space counters.
   */
  for(unsigned int i = 0; i < gen - 1; i++) {

    gc_used_space[i].value = gc_new_space[i].value;
    gc_new_space[i].value = 0;

  }

  /* Set the new space to NULL.  This only gets built during a collection */
  for(unsigned int i = 0; i < SLICE_POWERS; i++)
    for(unsigned int j = 0; j < gen - 1; j++)
      gc_new_slices[i][j].value = NULL;

  store_fence();

}
