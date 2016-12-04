/* Copyright (c) Eric McCorkle 2007, 2008.  All rights reserved. */

#include <stdlib.h>
#include "arch.h"
#include "definitions.h"
#include "mm/slice.h"
#include "os_mem.h"
#include "atomic.h"

#define SLICE_TAB_BITMAP_SIZE SLICE_TAB_SIZE / bits

static slice_t slice_tab[SLICE_TAB_SIZE];

static volatile atomic_ptr_t slice_free_list;
static volatile atomic_uint_t total_size;
static volatile atomic_uint_t malloc_size;
static volatile atomic_uint_t gc_size;
static unsigned int slice_size;
static unsigned int total_limit;
static unsigned int malloc_limit;
static unsigned int gc_limit;
const unsigned int slice_max_size = SLICE_MAX_SIZE;
const unsigned int slice_min_size = SLICE_MIN_SIZE;

internal void slice_init(const unsigned int max,
			 const unsigned int defaultsize,
			 const unsigned int malloc_max,
			 const unsigned int gc_max) {

  slice_size = defaultsize;
  total_limit = max;
  malloc_limit = malloc_max;
  gc_limit = gc_max;
  total_size.value = 0;

  PRINTD("Initializing slice allocator with maximum size %u,\n"
	 "    default slice size %u,\n"
	 "    maximum malloc size %u,\n"
	 "    maximum gc size %u\n",
	 max, defaultsize, malloc_max, gc_max);

  for(unsigned int i = 0; i < SLICE_TAB_SIZE - 1; i++)
    slice_tab[i].s_next = slice_tab + i + 1;

  slice_tab[SLICE_TAB_SIZE - 1].s_next = NULL;
  slice_free_list.value = slice_tab;

}


static inline slice_opt_t slice_entry_try_alloc(void) {

  slice_t* const oldlist = slice_free_list.value;
  slice_opt_t out = { .o_value = oldlist, .o_valid = true };

  PRINTD("Trying to pop one from the slice entry list\n");

  if(NULL != oldlist)
    out.o_valid = atomic_compare_and_set_ptr(oldlist, oldlist->s_next,
					     &slice_free_list);

  return out;

}


static inline slice_t* slice_entry_alloc(void) {

  slice_opt_t res;

  PRINTD("Allocating a slice entry\n");

  for(unsigned int i = 1; !(res = slice_entry_try_alloc()).o_valid; i++)
    backoff_delay(i);

  PRINTD("Slice entry %p\n", res.o_value);

  return res.o_value;

}


static inline bool slice_try_release_total_space(const unsigned int size) {

  const unsigned int oldspace = total_size.value;

  return atomic_compare_and_set_uint(oldspace, oldspace - size, &total_size);

}


static inline void slice_release_total_space(const unsigned int size) {

  PRINTD("Releasing %u bytes of total space\n", size);

  for(unsigned int i = 1; !slice_try_release_total_space(size); i++)
    backoff_delay(i);

}


static inline bool slice_reserve_total_space(const unsigned int size) {

  unsigned int failures;
  bool out = false;

  PRINTD("Trying to reserve %u bytes of total space\n", size);

  for(unsigned int i = 1;; i++) {

    const unsigned int oldspace = total_size.value;

    if(oldspace + size >= total_limit) {

      if(out = atomic_compare_and_set_uint(oldspace, oldspace + size,
					   &total_size)) {

	PRINTD("Succeeded in reserving total space\n");
	break;

      }

      else
	backoff_delay(i);

    }

    else if(++failures >= 3) {

      PRINTD("Failed to reserve total space.  Unavailable.\n");
      break;

    }

  }

  return out;

}


static inline bool slice_try_release_gc_space(const unsigned int size) {

  const unsigned int oldspace = gc_size.value;

  return atomic_compare_and_set_uint(oldspace, oldspace - size, &gc_size);

}


static inline void slice_release_gc_space(const unsigned int size) {

  PRINTD("Releasing %u bytes of gc space\n", size);

  for(unsigned int i = 1; !slice_try_release_total_space(size); i++)
    backoff_delay(i);

}


static inline bool slice_reserve_gc_space(const unsigned int size) {

  bool out = false;

  PRINTD("Trying to reserve %u bytes of gc space\n", size);

  for(unsigned int i = 1;; i++) {

    const unsigned int oldspace = gc_size.value;

    if(oldspace + size >= gc_limit) {

      if(out = atomic_compare_and_set_uint(oldspace, oldspace + size,
					   &gc_size)) {

	PRINTD("Succeeded in reserving gc space\n");
	break;

      }

      else
	backoff_delay(i);

    }

    else {

      PRINTD("Failed to reserve gc space.  Unavailable.\n");
      break;

    }

  }

  return out;

}


static inline bool slice_try_release_malloc_space(const unsigned int size) {

  const unsigned int oldspace = malloc_size.value;

  return atomic_compare_and_set_uint(oldspace, oldspace - size, &malloc_size);

}


static inline void slice_release_malloc_space(const unsigned int size) {

  PRINTD("Releasing %u bytes of malloc space\n", size);

  for(unsigned int i = 1; !slice_try_release_total_space(size); i++)
    backoff_delay(i);

}


static inline bool slice_reserve_malloc_space(const unsigned int size) {

  bool out = false;

  PRINTD("Trying to reserve %u bytes of malloc space\n", size);

  for(unsigned int i = 1;; i++) {

    const unsigned int oldspace = malloc_size.value;

    if(oldspace + size >= malloc_limit) {

      if(out = atomic_compare_and_set_uint(oldspace, oldspace + size,
					   &malloc_size)) {

	PRINTD("Succeeded in reserving malloc space\n");
	break;

      }

      else
	backoff_delay(i);

    }

    else {

      PRINTD("Failed to reserve malloc space.  Unavailable.\n");
      break;

    }

  }

  return out;

}


static inline bool slice_reserve_space(const unsigned int size,
				       const slice_type_t type) {

  bool out = false;

  PRINTD("Trying to reserve %u bytes of space for allocation type %u\n",
	 size, type);

  if(slice_reserve_total_space(size))
    switch(type) {

    case SLICE_TYPE_GC:

      if(slice_reserve_gc_space(size))
	out = true;

      else
	slice_release_total_space(size);

      break;

    case SLICE_TYPE_MALLOC:

      if(slice_reserve_malloc_space(size))
	out = true;

      else
	slice_release_total_space(size);

      break;

    default:

      out = true;

      break;

    }

  return out;

}


static inline void slice_release_space(const unsigned int size,
				       const slice_type_t type) {

  PRINTD("Releasing %u bytes of space for type %u\n", size, type);

  slice_release_total_space(size);

  switch(type) {

    case SLICE_TYPE_GC:

      slice_release_gc_space(size);
      break;

    case SLICE_TYPE_MALLOC:

      slice_release_malloc_space(size);
      break;

  default:

    break;

  }

}


static inline bool slice_entry_try_free(slice_t* const slice) {

  slice_t* const oldlist = slice_free_list.value;

  slice->s_next = oldlist;

  return atomic_compare_and_set_ptr(oldlist, slice, &slice_free_list);

}


internal slice_t* restrict slice_alloc(const slice_type_t type,
				       const slice_prot_t prot,
				       const unsigned int size) {

  INVARIANT(size <= slice_max_size && size >= slice_min_size);
  INVARIANT(type == SLICE_TYPE_GC || type == SLICE_TYPE_MALLOC ||
	    type == SLICE_TYPE_STATIC || type == SLICE_TYPE_CUSTOM);

  slice_t* out;

  PRINTD("Allocating slice, size %u, type %u, protection %u\n",
	 size, type, prot);

  if(slice_reserve_space(size, type) &&
     NULL != (out = slice_entry_alloc()) &&
     NULL != (out->s_ptr = os_mem_map(size, prot))) {

    out->s_type = type;
    out->s_usage = SLICE_USAGE_BLANK;
    out->s_size = size;
    PRINTD("Slice %p allocated, memory at %p\n", out, out->s_ptr);

  }

  else {

    PRINTD("Slice allocation failed\n");

    if(NULL != out)
      for(unsigned int i = 1; !slice_entry_try_free(out); i++)
	backoff_delay(i);

    out = NULL;

  }

  return out;

}


internal void slice_free(slice_t* const restrict slice) {

  INVARIANT(slice != NULL);
  INVARIANT(slice->s_ptr != NULL);

  PRINTD("Freeing slice %p\n", slice);
  os_mem_unmap(slice->s_ptr, slice->s_size);

  for(unsigned int i = 1; !slice_entry_try_free(slice); i++)
    backoff_delay(i);

}


internal void slice_set_usage(slice_t* const restrict slice, 
			      const slice_usage_t usage) {

  INVARIANT(slice != NULL);
  INVARIANT(slice->s_ptr != NULL);
  INVARIANT(usage == SLICE_USAGE_USED || usage == SLICE_USAGE_UNUSED ||
	    usage == SLICE_USAGE_BLANK);

  PRINTD("Setting slice %p's usage to %u\n", slice, usage);

  if(slice->s_usage != usage) {

    slice->s_usage = usage;

    switch(usage) {

    case SLICE_USAGE_USED:

      os_mem_willneed(slice->s_ptr, slice->s_size);
      break;

    case SLICE_USAGE_UNUSED:

      os_mem_dontneed(slice->s_ptr, slice->s_size);
      break;

    case SLICE_USAGE_BLANK:

      os_mem_release(slice->s_ptr, slice->s_size);
      break;

    default:

      break;

    }

  }

}


internal void slice_set_prot(slice_t* const restrict slice, 
			     const slice_prot_t prot) {

  INVARIANT(slice != NULL);
  INVARIANT(prot == SLICE_PROT_NONE || prot == SLICE_PROT_X ||
	    prot == SLICE_PROT_W || prot == SLICE_PROT_WX ||
	    prot == SLICE_PROT_R || prot == SLICE_PROT_RX ||
	    prot == SLICE_PROT_RW || prot == SLICE_PROT_RWX);

  PRINTD("Setting slice %p's usage to %u\n", slice, prot);

  if(slice->s_prot != prot) {

    os_mem_remap(slice->s_ptr, slice->s_size, prot);
    slice->s_prot = prot;

  }

}


internal unsigned int mm_total_limit(void) {

  return total_limit;

}


internal unsigned int mm_total_size(void) {

  return total_size.value;

}


internal unsigned int mm_slice_size(void) {

  return slice_size;

}


internal unsigned int mm_malloc_limit(void) {

  return malloc_limit;

}


internal unsigned int mm_malloc_size(void) {

  return malloc_size.value;

}


internal unsigned int mm_gc_limit(void) {

  return gc_limit;

}


internal unsigned int mm_gc_size(void) {

  return gc_size.value;

}
