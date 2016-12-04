/* Copyright (c) 2007 Eric McCorkle.  All rights reserved. */

#include <stdbool.h>

#include "definitions.h"
#include "mm.h"
#include "mm/mm_malloc.h"
#include "mm/gc_thread.h"

#include "../os/os_mem.c"
#include "slice.c"

#include "arch/lf_malloc_data.c"
#include "arch/bitops.c"
#include "malloc/lf_block_queue.c"
#include "malloc/lf_malloc.c"
#include "gc/gc_desc.c"
#include "gc/gc_alloc.c"
#include "gc/lf_object_queue.c"
#include "gc/gc_thread.c"

const char* const mm_name = "Parallel Lock-Free Generational";

unsigned int mm_request(mm_stat_t* const restrict mm_stat,
			const cc_stat_t* const restrict cc_stat) {

  PRINTD("Reserving space for memory manager\n");

  const unsigned int malloc_request =
    mm_malloc_request(cc_stat->cc_num_executors);
  const unsigned int gc_request =
    gc_thread_request(cc_stat->cc_num_executors,
		      mm_stat->mm_num_generations);

  PRINTD("  Reserving 0x%x bytes for malloc system.\n", malloc_request);
  PRINTD("  Reserving 0x%x bytes for GC system.\n", gc_request);
  PRINTD("Memory manager total static size is 0x%x bytes.\n",
	 malloc_request + gc_request);

  return malloc_request + gc_request;

}

void* mm_start(const mm_stat_t* const mm_stat,
	       const cc_stat_t* const cc_stat,
	       const unsigned int size) {

  const unsigned int align_request =
    slice_min_size > size ? slice_min_size : size;
  const slice_t* static_data;
  void* out = NULL;

  slice_init(mm_stat->mm_total_limit, mm_stat->mm_slice_size,
	     mm_stat->mm_malloc_limit, mm_stat->mm_gc_limit);
  PRINTD("Allocating static data slice\n");
  static_data = slice_alloc(SLICE_TYPE_STATIC, SLICE_PROT_RWX, align_request);
  PRINTD("Static data starts at %p\n", static_data->s_ptr);

  if(NULL != static_data) {

    void* const mem = static_data->s_ptr;
    void* const ptr = mm_malloc_init(cc_stat->cc_num_executors, mem);

    out = gc_thread_init(cc_stat->cc_num_executors,
			 mm_stat->mm_num_generations, ptr);

    PRINTD("Memory system memory:\n");
    PRINTD("\tmalloc system at 0x%p\n", mem);
    PRINTD("\tGC system at 0x%p\n", ptr);
    PRINTD("\tend at 0x%p\n", out);

  }

  else {

    perror("Cannot allocate initial memory for runtime:");
    exit(-1);

  }

  return out;

}

#ifdef NOT
mm_stat_t mm_stat(void) {

  mm_stat_t out;

  out.mm_total_limit = mm_total_limit();
  out.mm_gc_limit = mm_gc_limit();
  out.mm_malloc_limit = mm_malloc_limit();
  out.mm_slice_size = mm_slice_size();
  out.mm_total_size = mm_total_size();
  out.mm_gc_size = mm_gc_size();
  out.mm_malloc_size = mm_malloc_size();
  out.mm_gc_live = mm_gc_live();
  out.mm_malloc_live = mm_malloc_live();
  out.mm_total_live = out.mm_malloc_live + out.mm_gc_live;

  return out;

}


static bool try_mm_stat_snap(mm_stat_t* restrict stats) {

  stats->mm_total_size = mm_total_size();
  stats->mm_gc_size = mm_gc_size();
  stats->mm_malloc_size = mm_malloc_size();
  stats->mm_gc_live = mm_gc_live();
  stats->mm_malloc_live = mm_malloc_live();

  return stats->mm_total_size == mm_total_size() &&
    stats->mm_gc_size == mm_gc_size() &&
    stats->mm_malloc_size == mm_malloc_size() &&
    stats->mm_gc_live == mm_gc_live() &&
    stats->mm_malloc_live == mm_malloc_live();

}


mm_stat_t mm_stat_snap(void) {

  mm_stat_t out;

  out.mm_total_limit = mm_total_limit();
  out.mm_gc_limit = mm_gc_limit();
  out.mm_malloc_limit = mm_malloc_limit();
  out.mm_slice_size = mm_slice_size();

  for(unsigned int i = 0; !try_mm_stat_snap(&out); i++)
    backoff_delay(i);

  out.mm_total_live = out.mm_malloc_live + out.mm_gc_live;

  return out;

}


void mm_compact(unsigned int size) {

  const unsigned int slice_size = mm_slice_size();

  gc_collect_major();

  /* If the desired size is small, or there is not enough memory, then
   * compact as tightly as possible.
   */
  if(size < mm_slice_size() || mm_malloc_live() + mm_gc_live() < size) {

    malloc_compact(0);
    gc_compact(0);

  }

  else {

    const int total_dead = mm_total_size() - (mm_malloc_live() + mm_gc_live());
    const unsigned int total_to_free =
      total_dead > 0 ? uint_floor(total_dead, slice_size) : 0;
    const unsigned int malloc_size = mm_malloc_size();
    const unsigned int gc_size = mm_gc_size();
    const float free_frac = (1.0 * size) / (1.0 * total_to_free);
    const unsigned int malloc_target_size =
      uint_floor(free_frac * malloc_size, slice_size);
    const unsigned int gc_target_size =
      uint_floor(free_frac * gc_size, slice_size);

    /* If one store doesn't have enough space, compact it as tightly
     * as possible, and try to make up with the other.  Otherwise,
     * just go to the target value.
     */
    if(malloc_size > malloc_target_size) {

      const int malloc_dead = mm_malloc_size() - mm_malloc_live();
      const unsigned int malloc_to_free =
	malloc_dead > 0 ? uint_floor(malloc_dead, slice_size) : 0;
      const unsigned int leftover = total_to_free - malloc_to_free;

      malloc_compact(malloc_size);
      gc_compact(gc_target_size - leftover);

    }

    else if(gc_size > gc_target_size) {

      const int gc_dead = mm_gc_size() - mm_gc_live();
      const unsigned int gc_to_free =
	gc_dead > 0 ? uint_floor(gc_dead, slice_size) : 0;
      const unsigned int leftover = total_to_free - gc_to_free;

      gc_compact(gc_size);
      malloc_compact(malloc_target_size - leftover);

    }

    else {

      malloc_compact(malloc_target_size);
      gc_compact(gc_target_size);

    }

  }

}
#endif
