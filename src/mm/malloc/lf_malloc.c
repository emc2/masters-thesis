/* Copyright (c) 2007, 2008 Eric McCorkle.  All rights reserved. */

#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "definitions.h"
#include "atomic.h"
#include "malloc.h"
#include "cc.h"
#include "mm/slice.h"
#include "mm/lf_malloc_data.h"
#include "mm/lf_block_queue.h"
#include "mm/mm_malloc.h"

/* Scalable Lock-Free Dynamic Memory Allocation, by Maged D. Michael */

typedef struct descriptor_t descriptor_t;
typedef struct procheap_t procheap_t;
typedef struct sizeclass_t sizeclass_t;

struct descriptor_t {

  volatile atomic_uint64_t des_anchor;
  descriptor_t* des_next;
  slice_t* des_slice;
  procheap_t* des_heap;
  unsigned int des_size;
  unsigned int des_maxcount;

};

struct procheap_t {

  volatile atomic_ptr_t ph_partial;
  volatile atomic_uint64_t ph_active;
  sizeclass_t* ph_sizeclass;

};

struct sizeclass_t {

  unsigned int sc_size;
  unsigned int sc_block_size;
  lf_block_queue_t* sc_partial;

};

static const unsigned int ANC_ACTIVE = 0;
static const unsigned int ANC_FULL = 1;
static const unsigned int ANC_PARTIAL= 2;
static const unsigned int ANC_EMPTY = 3;

static inline unsigned int malloc_big_size_class(const unsigned int size,
						 const unsigned int base) {

  INVARIANT(size <= 0x4000);

  const unsigned int size_div128 =
    size >> 6;
  unsigned int out = 0;

  if(4 > size_div128)
    out = base + size_div128;

  else {

    const unsigned int size_div256 =
      (size_div128 - 4) >> 1;

    if(4 > size_div256)
      out = base + 4 + size_div256;

    else {

      const unsigned int size_div512 =
	(size_div256 - 4) >> 1;

      if(4 > size_div512)
	out = base + 8 + size_div512;

      else {

	const unsigned int size_div1024 =
	  (size_div512 - 4) >> 1;

	if(4 > size_div1024)
	  out = base + 12 + size_div1024;

	else {

	  const unsigned int size_div2048 =
	    (size_div1024 - 4) >> 1;

	  if(4 > size_div2048)
	    out = base + 16 + size_div2048;

	  else {

	    const unsigned int size_div4096 =
	      (size_div2048 - 4) >> 1;

	    out = base + 20 + size_div4096;

	  }

	}

      }

    }

  }

  return out;

}

#if (CACHE_LINE_SIZE <= 16)
#define NUM_SIZE_CLASSES 36

static sizeclass_t malloc_sizeclasses[NUM_SIZE_CLASSES] = {
  { .sc_size = 0x10, .sc_block_size = 0x10000 },
  { .sc_size = 0x20, .sc_block_size = 0x10000 },
  { .sc_size = 0x30, .sc_block_size = 0x10000 },
  { .sc_size = 0x40, .sc_block_size = 0x10000 },
  { .sc_size = 0x50, .sc_block_size = 0x10000 },
  { .sc_size = 0x60, .sc_block_size = 0x10000 },
  { .sc_size = 0x70, .sc_block_size = 0x10000 },
  { .sc_size = 0x80, .sc_block_size = 0x10000 },
  { .sc_size = 0xa0, .sc_block_size = 0x10000 },
  { .sc_size = 0xc0, .sc_block_size = 0x10000 },
  { .sc_size = 0xe0, .sc_block_size = 0x10000 },
  { .sc_size = 0x100, .sc_block_size = 0x10000 },
  { .sc_size = 0x140, .sc_block_size = 0x10000 },
  { .sc_size = 0x180, .sc_block_size = 0x10000 },
  { .sc_size = 0x1c0, .sc_block_size = 0x10000 },
  { .sc_size = 0x200, .sc_block_size = 0x10000 },
  { .sc_size = 0x280, .sc_block_size = 0x10000 },
  { .sc_size = 0x300, .sc_block_size = 0x10000 },
  { .sc_size = 0x380, .sc_block_size = 0x10000 },
  { .sc_size = 0x400, .sc_block_size = 0x10000 },
  { .sc_size = 0x500, .sc_block_size = 0x10000 },
  { .sc_size = 0x600, .sc_block_size = 0x10000 },
  { .sc_size = 0x700, .sc_block_size = 0x10000 },
  { .sc_size = 0x800, .sc_block_size = 0x10000 },
  { .sc_size = 0xa00, .sc_block_size = 0x10000 },
  { .sc_size = 0xc00, .sc_block_size = 0x10000 },
  { .sc_size = 0xe00, .sc_block_size = 0x10000 },
  { .sc_size = 0x1000, .sc_block_size = 0x10000 },
  { .sc_size = 0x1400, .sc_block_size = 0x10000 },
  { .sc_size = 0x1800, .sc_block_size = 0x10000 },
  { .sc_size = 0x1c00, .sc_block_size = 0x10000 },
  { .sc_size = 0x2000, .sc_block_size = 0x10000 },
  { .sc_size = 0x2800, .sc_block_size = 0x10000 },
  { .sc_size = 0x3000, .sc_block_size = 0x10000 },
  { .sc_size = 0x3800, .sc_block_size = 0x10000 },
  { .sc_size = 0x4000, .sc_block_size = 0x10000 }
};


static inline unsigned int malloc_size_class(const unsigned int size) {

  INVARIANT(size <= 0x4000);

  const unsigned int size_div16 =
    size >> 4;
  unsigned int out = 0;

  if(8 > size_div16)
    out = size_div16;

  else {

    const unsigned int size_div32 =
      (size_div16 - 8) >> 1;

    if(4 > size_div32)
      out = 8 + size_div32;

    else
      out = malloc_big_size_class(size, 12);

  }

  return out;

}


#elif (CACHE_LINE_SIZE <= 32)
#define NUM_SIZE_CLASSES 32

static sizeclass_t malloc_sizeclasses[NUM_SIZE_CLASSES] = {
  { .sc_size = 0x20, .sc_block_size = 0x10000 },
  { .sc_size = 0x40, .sc_block_size = 0x10000 },
  { .sc_size = 0x60, .sc_block_size = 0x10000 },
  { .sc_size = 0x80, .sc_block_size = 0x10000 },
  { .sc_size = 0xa0, .sc_block_size = 0x10000 },
  { .sc_size = 0xc0, .sc_block_size = 0x10000 },
  { .sc_size = 0xe0, .sc_block_size = 0x10000 },
  { .sc_size = 0x100, .sc_block_size = 0x10000 },
  { .sc_size = 0x140, .sc_block_size = 0x10000 },
  { .sc_size = 0x180, .sc_block_size = 0x10000 },
  { .sc_size = 0x1c0, .sc_block_size = 0x10000 },
  { .sc_size = 0x200, .sc_block_size = 0x10000 },
  { .sc_size = 0x280, .sc_block_size = 0x10000 },
  { .sc_size = 0x300, .sc_block_size = 0x10000 },
  { .sc_size = 0x380, .sc_block_size = 0x10000 },
  { .sc_size = 0x400, .sc_block_size = 0x10000 },
  { .sc_size = 0x500, .sc_block_size = 0x10000 },
  { .sc_size = 0x600, .sc_block_size = 0x10000 },
  { .sc_size = 0x700, .sc_block_size = 0x10000 },
  { .sc_size = 0x800, .sc_block_size = 0x10000 },
  { .sc_size = 0xa00, .sc_block_size = 0x10000 },
  { .sc_size = 0xc00, .sc_block_size = 0x10000 },
  { .sc_size = 0xe00, .sc_block_size = 0x10000 },
  { .sc_size = 0x1000, .sc_block_size = 0x10000 },
  { .sc_size = 0x1400, .sc_block_size = 0x10000 },
  { .sc_size = 0x1800, .sc_block_size = 0x10000 },
  { .sc_size = 0x1c00, .sc_block_size = 0x10000 },
  { .sc_size = 0x2000, .sc_block_size = 0x10000 },
  { .sc_size = 0x2800, .sc_block_size = 0x10000 },
  { .sc_size = 0x3000, .sc_block_size = 0x10000 },
  { .sc_size = 0x3800, .sc_block_size = 0x10000 },
  { .sc_size = 0x4000, .sc_block_size = 0x10000 }
};

static inline unsigned int malloc_size_class(const unsigned int size) {

  INVARIANT(size <= 0x4000);

  const unsigned int size_div32 =
    size >> 5;
  unsigned int out = 0;

  if(8 > size_div32)
    out = size_div32;

  else
    out = malloc_big_size_class(size, 8);

  return out;

}

#else
#define NUM_SIZE_CLASSES 28

static sizeclass_t malloc_sizeclasses[NUM_SIZE_CLASSES] = {
  { .sc_size = 0x40, .sc_block_size = 0x10000 },
  { .sc_size = 0x80, .sc_block_size = 0x10000 },
  { .sc_size = 0xc0, .sc_block_size = 0x10000 },
  { .sc_size = 0x100, .sc_block_size = 0x10000 },
  { .sc_size = 0x140, .sc_block_size = 0x10000 },
  { .sc_size = 0x180, .sc_block_size = 0x10000 },
  { .sc_size = 0x1c0, .sc_block_size = 0x10000 },
  { .sc_size = 0x200, .sc_block_size = 0x10000 },
  { .sc_size = 0x280, .sc_block_size = 0x10000 },
  { .sc_size = 0x300, .sc_block_size = 0x10000 },
  { .sc_size = 0x380, .sc_block_size = 0x10000 },
  { .sc_size = 0x400, .sc_block_size = 0x10000 },
  { .sc_size = 0x500, .sc_block_size = 0x10000 },
  { .sc_size = 0x600, .sc_block_size = 0x10000 },
  { .sc_size = 0x700, .sc_block_size = 0x10000 },
  { .sc_size = 0x800, .sc_block_size = 0x10000 },
  { .sc_size = 0xa00, .sc_block_size = 0x10000 },
  { .sc_size = 0xc00, .sc_block_size = 0x10000 },
  { .sc_size = 0xe00, .sc_block_size = 0x10000 },
  { .sc_size = 0x1000, .sc_block_size = 0x10000 },
  { .sc_size = 0x1400, .sc_block_size = 0x10000 },
  { .sc_size = 0x1800, .sc_block_size = 0x10000 },
  { .sc_size = 0x1c00, .sc_block_size = 0x10000 },
  { .sc_size = 0x2000, .sc_block_size = 0x10000 },
  { .sc_size = 0x2800, .sc_block_size = 0x10000 },
  { .sc_size = 0x3000, .sc_block_size = 0x10000 },
  { .sc_size = 0x3800, .sc_block_size = 0x10000 },
  { .sc_size = 0x4000, .sc_block_size = 0x10000 }
};

static inline unsigned int malloc_size_class(const unsigned int size) {

  INVARIANT(size <= 0x4000);

  const unsigned int size_div64 =
    size >> 6;
  unsigned int out = 0;

  if(4 > size_div64)
    out = size_div64;

  else
      out = malloc_big_size_class(size, 4);

  return out;

}

#endif

static volatile atomic_ptr_t malloc_desc_avail;

static procheap_t (*malloc_procheaps)[NUM_SIZE_CLASSES];


internal unsigned int mm_malloc_request(const unsigned int execs) {

  INVARIANT(execs != 0);

  PRINTD("  Reserving space for malloc system\n");


  const unsigned int procheap_size =
    sizeof(procheap_t) * execs * NUM_SIZE_CLASSES;
  const unsigned int procheap_aligned_size =
    ((procheap_size - 1) & ~(CACHE_LINE_SIZE - 1)) + CACHE_LINE_SIZE;
  const unsigned int one_blockqueue_size =
    sizeof(lf_block_queue_t) + (execs * sizeof(lf_block_queue_hazard_ptrs_t));
  const unsigned int one_blockqueue_aligned_size =
    ((one_blockqueue_size - 1) & ~(CACHE_LINE_SIZE - 1)) + CACHE_LINE_SIZE;
  const unsigned int blockqueues_aligned_size =
    one_blockqueue_aligned_size * NUM_SIZE_CLASSES;
  const unsigned int total_size =
    procheap_aligned_size + blockqueues_aligned_size;

  PRINTD("    Reserving 0x%x bytes for processor heaps.\n",
	 procheap_size);
  PRINTD("    Reserving 0x%x bytes for block queues.\n",
	 blockqueues_aligned_size);
  PRINTD("  Malloc system total static size is 0x%x bytes.\n", total_size);

  return total_size;

}


internal void* mm_malloc_init(const unsigned int execs, void* const mem) {

  INVARIANT(execs != 0);
  INVARIANT(mem != NULL);

  PRINTD("Initializing malloc system, static memory at 0x%p.\n", mem);

  const unsigned int procheap_size =
    sizeof(procheap_t) * execs * NUM_SIZE_CLASSES;
  const unsigned int procheap_aligned_size =
    ((procheap_size - 1) & ~(CACHE_LINE_SIZE - 1)) + CACHE_LINE_SIZE;
  const unsigned int one_blockqueue_size =
    sizeof(lf_block_queue_t) + (execs * sizeof(lf_block_queue_hazard_ptrs_t));
  const unsigned int one_blockqueue_aligned_size =
    ((one_blockqueue_size - 1) & ~(CACHE_LINE_SIZE - 1)) + CACHE_LINE_SIZE;
  procheap_t (* const procheaps_ptr)[NUM_SIZE_CLASSES] = mem;
  char* const queues_ptr = (char*)mem + procheap_aligned_size;
  char* const out = queues_ptr +
    (one_blockqueue_aligned_size * NUM_SIZE_CLASSES);

  PRINTD("Malloc system memory:\n");
  PRINTD("\tprocessor heaps at 0x%p\n", mem);
  PRINTD("\tblock queues at 0x%p\n", queues_ptr);
  PRINTD("\tend at 0x%p\n", out);

  PRINTD("Initializing malloc system with %u executors and memory at %p\n",
	 execs, mem);
  malloc_procheaps = procheaps_ptr;

  for(unsigned int i = 0; i < execs; i++)
    for(unsigned int j = 0; j < NUM_SIZE_CLASSES; j++) {

      PRINTD("Processor heap for sizeclass %u for executor %u at %p\n",
	     j, i, malloc_procheaps[i] + j);
      malloc_procheaps[i][j].ph_partial.value = NULL;
      malloc_procheaps[i][j].ph_active.value = 0;
      malloc_procheaps[i][j].ph_sizeclass = malloc_sizeclasses + j;

    }

  for(unsigned int i = 0; i < NUM_SIZE_CLASSES; i++) {

    lf_block_queue_t* const queue =
      (lf_block_queue_t*)(queues_ptr + (i * one_blockqueue_aligned_size));

    PRINTD("Block queue for sizeclass %u at %p\n", i, queue);
    malloc_sizeclasses[i].sc_partial = queue;
    lf_block_queue_init(queue, execs);

  }

  return out;

}


static inline descriptor_t* try_desc_alloc(void) {

  descriptor_t* const desc = malloc_desc_avail.value;
  descriptor_t* out = NULL;

  PRINTD("Attempting to allocate a descriptor\n");

  if(NULL != desc) {

    descriptor_t* const next = desc->des_next;

    PRINTD("Descriptors are already available\n");

    if(atomic_compare_and_set_ptr(desc, next, &malloc_desc_avail))
      out = desc;

  }

  else {

    /* XXX do a two-level slice allocation */
    slice_t* const slice =
      slice_alloc(SLICE_TYPE_MALLOC, SLICE_PROT_RWX, 0x10000);

    PRINTD("No descriptors exist, allocating more\n");

    if(NULL != slice) {

      const unsigned int dessize =
	sizeof(descriptor_t) < 64 ? 64 : sizeof(descriptor_t);
      const unsigned int count = slice->s_size / dessize;
      descriptor_t* addr = slice->s_ptr;
      descriptor_t* newlist = (void*)((char*)addr + dessize);

      PRINTD("Arranging descriptors in a list\n");

      /* organize descriptors in a list */
      for(unsigned int i = 0; i < count - 1; i++) {

	addr->des_next = (void*)((char*)addr + dessize);
	addr = (void*)((char*)addr + dessize);

      }

      addr = (void*)((char*)addr - dessize);
      addr->des_next = NULL;
      store_fence();

      if(atomic_compare_and_set_ptr(NULL, newlist, &malloc_desc_avail)) {

	PRINTD("Succeded in setting available descriptors\n");
	out = addr;

      }

      else {

	PRINTD("Someone else created descriptors\n");
	slice_free(slice);

      }

    }

  }

  PRINTD("Descriptor allocation returned %p\n", out);

  return out;

}


static inline descriptor_t* desc_alloc(void) {

  descriptor_t* out;

  PRINTD("Allocating a descriptor\n");

  for(unsigned int i = 1; NULL == (out = try_desc_alloc()); i++)
    backoff_delay(i);

  PRINTD("Descriptor is %p\n", out);

  return out;

}


static inline bool malloc_try_desc_retire(descriptor_t* const desc) {

  descriptor_t* const oldhead = malloc_desc_avail.value;

  PRINTD("Trying to retire malloc descriptor %p\n", desc);

  desc->des_next = oldhead;
  store_fence();

  return atomic_compare_and_set_ptr(oldhead, desc, &malloc_desc_avail);

}


static inline void malloc_desc_retire(descriptor_t* const desc) {

  for(unsigned int i = 1; ! malloc_try_desc_retire(desc); i++)
    backoff_delay(i);

}


/* Get a procheap structure for this executor and size class. */
static inline procheap_t* find_heap(const unsigned int size,
				    const unsigned int exec) {

  procheap_t* out;

  PRINTD("Finding heap for size %u for executor %u\n", size, exec);

  if(size <= 0x40000) {

    const unsigned int size_class = malloc_size_class(size);

    PRINTD("Block size %u assigned size class %u (size %u)\n",
	   size, size_class, malloc_sizeclasses[size_class].sc_size);
    out = malloc_procheaps[exec] + size_class;

  }

  else
    out = NULL;

  return out;

}


static inline bool try_reserve_from_active(procheap_t* const restrict procheap,
					   const active_t active) {

  unsigned int credits;
  active_t newactive;

  if(0 == (credits = active_get_credits(active)))
    newactive = active_set_ptr(active, NULL);

  else
    newactive = active_set_credits(active, credits - 1);

  return atomic_compare_and_set_uint64(active, newactive,
				       &(procheap->ph_active));

}


static inline active_t
reserve_from_active(procheap_t* const restrict procheap) {

  active_t active;

  for(unsigned int i = 0;; i++) {

    active = atomic_read_uint64(&(procheap->ph_active));

    if(NULL != active_get_ptr(active)) {

      if(try_reserve_from_active(procheap, active))
	 break;

      else
	backoff_delay(i);

    }

    else
      break;

  }

  return active;

}


static inline bool try_heap_put_partial(descriptor_t* const desc,
					const unsigned int exec) {

  procheap_t* const heap = desc->des_heap;
  descriptor_t* const prev = heap->ph_partial.value;
  bool out;

  PRINTD("Executor %d inserting partially complete descriptor "
	 "%p into heap %p\n", exec, desc, heap);

  if(out = atomic_compare_and_set_ptr(prev, desc,
				      &(desc->des_heap->ph_partial)))
    if(NULL != prev)
      lf_block_queue_enqueue(heap->ph_sizeclass->sc_partial, prev, exec);

  return out;

}


static inline void heap_put_partial(descriptor_t* const desc,
				    const unsigned int exec) {

  /* Modified the original algorithm to do exponential backoff */
  for(unsigned int i = 1; !try_heap_put_partial(desc, exec); i++)
      backoff_delay(i);

}


static inline bool update_partial(descriptor_t* const restrict desc,
				  const unsigned int morecredits,
				  const unsigned int exec) {

  const anchor_t oldanchor = atomic_read_uint64(&(desc->des_anchor));
  const anchor_t partial = anchor_set_state(oldanchor, ANC_PARTIAL);
  const anchor_t newanchor =
    anchor_set_credits(partial, anchor_get_credits(oldanchor) + morecredits);
  bool out;

  if(out = atomic_compare_and_set_uint64(oldanchor, newanchor,
					 &(desc->des_anchor)))
    heap_put_partial(desc, exec);

  return out;

}


static inline void update_active(procheap_t* const restrict heap,
				 descriptor_t* const restrict desc,
				 const unsigned int morecredits,
				 const unsigned int exec) {

  const active_t active = active_create(desc, morecredits - 1);

  if(!atomic_compare_and_set_uint64(0, active, &(heap->ph_active)))
    for(unsigned int i = 1;
	!update_partial(desc, morecredits, exec);
	i++)
      backoff_delay(i);

}


static inline void* pop_from_active(procheap_t* const restrict procheap,
				    const active_t active,
				    const unsigned int exec) {

  PRINTD("Executor %d pop from active: heap %p, active 0x%x\n",
	 exec, procheap, active);
  PRINTD("Active descriptor %p, credits %d\n",
	 active_get_ptr(active), active_get_credits(active));

  descriptor_t* const desc = active_get_ptr(active);
  const anchor_t oldanchor = atomic_read_uint64(&(desc->des_anchor));

  PRINTD("Anchor is %llx\nBlock %x next, slice pointer is %p, "
	 "size is %x, %d credits.\n",
	 oldanchor, anchor_get_avail(oldanchor),
	 desc->des_slice->s_ptr, desc->des_size,
	 anchor_get_credits(oldanchor));

  void* const addr = (char*)desc->des_slice->s_ptr +
    (anchor_get_avail(oldanchor) * desc->des_size);

  PRINTD("Address is %p\n", addr);

  const unsigned int next = *(unsigned int*)addr;
  const unsigned int credits = anchor_get_credits(oldanchor);
  const unsigned int morecredits = min(credits, MAX_CREDITS);
  const anchor_t avail = anchor_set_avail(oldanchor, next);
  anchor_t newanchor = anchor_set_tag(avail, anchor_get_tag(oldanchor) + 1);
  void* out;

  if(0 == active_get_credits(active)) {

    if(0 == anchor_get_credits(oldanchor))
      newanchor = anchor_set_state(newanchor, ANC_FULL);

    else {

      const unsigned int newcredits =
	anchor_get_credits(newanchor) - morecredits;

      newanchor = anchor_set_credits(newanchor, newcredits);

    }

  }

  if(atomic_compare_and_set_uint64(oldanchor, newanchor,
				   &(desc->des_anchor))) {

    void** const store_addr = addr;

    if(0 == active_get_credits(active) && 0 < anchor_get_credits(oldanchor))
      update_active(procheap, desc, morecredits, exec);

    *store_addr = desc;
    out = (char*)addr + CACHE_LINE_SIZE;

  }

  else
    out = NULL;

  return out;

}


static inline void* malloc_from_active(procheap_t* const restrict heap,
				       const unsigned int exec) {

  const active_t active = reserve_from_active(heap);
  void* out;

  if(NULL != active_get_ptr(active)) {

    for(unsigned int i = 1;
	NULL == (out = pop_from_active(heap, active, exec));
	i++)
      backoff_delay(i);

    PRINTD("Executor %u allocated %p from active\n", exec, out);

  }

  else {

    PRINTD("Executor %u failed to allocate from active\n", exec);
    out = NULL;

  }

  return out;

}


static inline descriptor_t* heap_get_partial(procheap_t* const restrict heap,
					     const unsigned int exec) {

  descriptor_t* out;

  for(unsigned int i = 0;; i++) {

    out = heap->ph_partial.value;

    if(NULL != out) {

      if(atomic_compare_and_set_ptr(out, NULL, &(heap->ph_partial)))
	break;

      else
	backoff_delay(i);

    }

    else {

      out = lf_block_queue_dequeue(heap->ph_sizeclass->sc_partial, exec);
      break;

    }

  }

  return out;

}


static inline void* pop_from_partial(descriptor_t* const restrict desc) {

  const anchor_t oldanchor = atomic_read_uint64(&(desc->des_anchor));
  void* const addr = (char*)desc->des_slice->s_ptr +
    (anchor_get_avail(oldanchor) * desc->des_size);
  const anchor_t avail = anchor_set_avail(oldanchor, *(unsigned int*)addr);
  const anchor_t newanchor =
    anchor_set_tag(avail, anchor_get_tag(oldanchor) + 1);

  return atomic_compare_and_set_uint64(oldanchor, newanchor,
				       &(desc->des_anchor)) ?
    addr : NULL;

}


static inline void* malloc_from_partial(procheap_t* const restrict heap,
					unsigned int exec) {

  descriptor_t* desc;
  unsigned int morecredits;
  void** addr;
  void* out;
  bool retry = true;

  for(unsigned int i = 0; retry; i++)
    if(NULL != (desc = heap_get_partial(heap, exec))) {

      for(;; i++) {

	const anchor_t oldanchor = atomic_read_uint64(&(desc->des_anchor));

	PRINTD("Anchor is %llx\nBlock %x next, slice pointer is %p, "
	       "size is %x, %d credits.\n",
	       oldanchor, anchor_get_avail(oldanchor),
	       desc->des_slice->s_ptr, desc->des_size,
	       anchor_get_credits(oldanchor));

	if(ANC_EMPTY != anchor_get_state(oldanchor)) {

	  const unsigned int credits =
	    min(anchor_get_credits(oldanchor) - 1, MAX_CREDITS);
	  const anchor_t active = anchor_set_state(oldanchor, credits > 0 ?
						   ANC_ACTIVE : ANC_FULL);
	  const anchor_t newanchor =
	    anchor_set_credits(active, anchor_get_credits(oldanchor) -
			       (credits + 1));

	  PRINTD("Partial descriptor %p now has %d blocks\n", desc, credits);

	  if(atomic_compare_and_set_uint64(oldanchor, newanchor,
					   &(desc->des_anchor))) {

	    PRINTD("Succeeded in claiming a partial descriptor\n");
	    morecredits = credits;
	    retry = false;
	    break;

	  }

	  else
	    backoff_delay(i);

	}

	else {

	  PRINTD("Partial heap was actually empty, retrying\n");
	  backoff_delay(i);
	  break;

	}

      }

    }

    else
      break;

  if(NULL != desc) {

    PRINTD("Attempting to take a block from partial descriptor %p\n", desc);

    for(unsigned int i = 1; NULL == (addr = pop_from_partial(desc)); i++)
      backoff_delay(i);

    if(morecredits > 0)
      update_active(heap, desc, morecredits, exec);

    *addr = desc;

    out = (char*)addr + CACHE_LINE_SIZE;
    PRINTD("Executor %u allocated %p from partial descriptor %p\n",
	   exec, out, desc);

  }

  else {

    PRINTD("Executor %u failed to allocate from partial\n", exec);
    out = NULL;

  }

  return out;

}

/* XXX need to do a retry */
static inline void* malloc_from_new_sb(procheap_t* const restrict heap) {

  /* XXX do a two-level slice allocation */
  slice_t* const slice = slice_alloc(SLICE_TYPE_MALLOC, SLICE_PROT_RWX,
				     heap->ph_sizeclass->sc_block_size);
  void* out;

  if(NULL != slice) {

    PRINTD("Allocated a new superblock\n");

    descriptor_t* const desc = desc_alloc();
    const unsigned int maxcount =
      heap->ph_sizeclass->sc_block_size / heap->ph_sizeclass->sc_size;
    const unsigned int credits = min(maxcount - 1, MAX_CREDITS) - 1;
    const active_t newactive = active_create(desc, credits);
    void* const addr = slice->s_ptr;

    PRINTD("Arranging blocks in a list\n");
    /* setup blocks in a list */
    for(unsigned int i = 1; i < maxcount - 1; i++) {

      const unsigned int offset = heap->ph_sizeclass->sc_size * i;
      unsigned int* const ptr = (unsigned int*)((char*)addr + offset);

      *ptr = i + 1;

    }

    desc->des_slice = slice;
    desc->des_heap = heap;
    desc->des_size = heap->ph_sizeclass->sc_size;
    desc->des_maxcount = maxcount;
    desc->des_anchor.value = anchor_create(1, (maxcount - 1) -
					   (credits + 1), ANC_ACTIVE);
    store_fence();

    if(atomic_compare_and_set_uint64(0, newactive, &(heap->ph_active))) {

      void** const slice_ptr = slice->s_ptr;

      PRINTD("Succeeded in setting the active to the new block\n");
      *slice_ptr = desc;
      out = (char*)slice_ptr + CACHE_LINE_SIZE;
      PRINTD("Allocated %p from a new superblock\n", out);

    }

    else {

      PRINTD("Failed to set the active to the new block");
      slice_free(slice);
      malloc_desc_retire(desc);
      out = NULL;

    }

  }

  else {

    PRINTD("Failed to allocate from a new superblock\n");
    out = NULL;

  }

  return out;

}


static inline void* try_alloc(procheap_t* const restrict heap,
			      const unsigned int exec) {

  INVARIANT(heap != NULL);

  void* out;

  PRINTD("Executor %u attempting allocation from active superblock\n", exec);

  if(NULL == (out = malloc_from_active(heap, exec))) {

    PRINTD("Executor %u attempting allocation from partial superblock\n", exec);

    if(NULL == (out = malloc_from_partial(heap, exec))) {

      PRINTD("Executor %u attempting allocation from new superblock\n", exec);
      out = malloc_from_new_sb(heap);

    }

  }

  return out;

}


static inline void remove_empty_desc(procheap_t* const heap,
				     descriptor_t* const desc,
				     const unsigned int exec) {

  PRINTD("Executor %d attempting to release empty descriptor %p.\n",
	 exec, desc);

  if(atomic_compare_and_set_ptr(desc, NULL, &(heap->ph_partial)))
    malloc_desc_retire(desc);

  else {

    lf_block_queue_t* const queue = heap->ph_sizeclass->sc_partial;
    descriptor_t* curr;

    while(NULL != (curr = lf_block_queue_dequeue(queue, exec)) &&
	  ANC_EMPTY !=
	  anchor_get_state(atomic_read_uint64(&(curr->des_anchor))));

    if(NULL != curr)
      lf_block_queue_enqueue(queue, curr, exec);

  }

}


static inline bool try_free(descriptor_t* const desc,
			    unsigned int* const ptr,
			    const unsigned int exec) {

  INVARIANT(desc != NULL);
  INVARIANT(ptr != NULL);

  const anchor_t oldanchor = atomic_read_uint64(&(desc->des_anchor));
  const unsigned int offset =
    ((char*)ptr - (char*)desc->des_slice->s_ptr) / desc->des_size;
  anchor_t newanchor =
    anchor_set_avail(ANC_FULL == anchor_get_state(oldanchor) ?
		     anchor_set_state(oldanchor, ANC_PARTIAL) : oldanchor,
		     offset);
  procheap_t* heap = NULL;
  bool out;

  PRINTD("Block is at offset %d\n", offset);
  PRINTD("Anchor is %llx\nBlock %x next, slice pointer is %p, "
	 "size is %x, %d credits.\n",
	 oldanchor, anchor_get_avail(oldanchor),
	 desc->des_slice->s_ptr, desc->des_size,
	 anchor_get_credits(oldanchor));
  PRINTD("Executor %d attempting to free block %p, descriptor %p.\n",
	 exec, ptr, desc);

  *ptr = anchor_get_avail(oldanchor);

  if(anchor_get_credits(oldanchor) == desc->des_maxcount - 1) {

    heap = desc->des_heap;
    load_fence();
    newanchor = anchor_set_state(newanchor, ANC_EMPTY);
    PRINTD("Executor %d set anchor to empty.\n", exec);

  }

  else {

    newanchor =
      anchor_set_credits(newanchor, anchor_get_credits(newanchor) + 1);
    PRINTD("Executor %d setting anchor's credits to %d.\n",
	   exec, anchor_get_credits(newanchor));


  }

  store_fence();

  if(out = atomic_compare_and_set_uint64(oldanchor, newanchor,
					 &(desc->des_anchor))) {

    PRINTD("Executor %d installed new anchor.\n", exec);

    if(ANC_EMPTY == anchor_get_state(newanchor)) {

      PRINTD("Executor %d releasing empty slice.\n", exec);
      slice_free(desc->des_slice);
      remove_empty_desc(heap, desc, exec);

    }

    else if(ANC_FULL == anchor_get_state(oldanchor)) {

      PRINTD("Executor %d inserting partially full slice.\n", exec);
      heap_put_partial(desc, exec);

    }

  }

  return out;

}


/* XXX reposition descriptor and don't add a cache line for objects
 * smaller than CACHE_LINE_SIZE - sizeof(void*)
 */
extern void* malloc_lf(const unsigned int req_size, const unsigned int exec) {

  void* out;

  PRINTD("Executor %u, malloc %d bytes\n", exec, req_size);

  if(0 != req_size) {

    const unsigned int size = req_size + CACHE_LINE_SIZE;
    procheap_t* const restrict heap = find_heap(size, exec);

    PRINTD("Full size %u\n", size);
    PRINTD("Allocating from heap %p\n", heap);

    /* Original algorithm modified to do exponential backoff */
    if(NULL != heap)
      for(unsigned int i = 1; NULL == (out = try_alloc(heap, exec)); i++)
	backoff_delay(i);

    else {

      const unsigned int slice_size = ((size - 1) % PAGE_SIZE) + PAGE_SIZE;
      /* XXX Go to a lock-free buddy-system allocator */
      const slice_t* const slice =
	slice_alloc(SLICE_TYPE_MALLOC, SLICE_PROT_RWX,
		    slice_size);

      PRINTD("Request was too big, allocated a whole slice of size %u\n",
	     slice_size);
      PRINTD("Slice address is %p\n", slice);

      if(NULL != slice) {

	unsigned int* const ptr = slice->s_ptr;
	const unsigned int ptr_val = (unsigned int)(slice->s_ptr) | 0x1;

	*(ptr) = ptr_val;
	out = (char*)ptr + CACHE_LINE_SIZE;

      }

      else
	out = NULL;

    }

  }

  else
    out = NULL;

  PRINTD("Executor %u, malloc %d bytes, result %p\n", exec, req_size, out);

  return out;

}


extern void free_lf(void* const ptr, const unsigned int exec) {

  PRINTD("Executor %u freeing %p\n", exec, ptr);

  if(NULL != ptr) {

    void* const prefix = (char*)ptr - CACHE_LINE_SIZE;
    const unsigned int tag_val = *((unsigned int*)prefix);
    const bool large_block = tag_val & 0x1;
    void* const tag_ptr = (void*)(tag_val & ~0x1);

    PRINTD("Prefix is at %p\n", prefix);
    PRINTD("Block %s large\n", large_block ? "is" : "is not");
    PRINTD("Tag pointer is %p\n", tag_ptr);

    if(!large_block)
      for(int i = 0; !try_free(tag_ptr, prefix, exec); i++)
	backoff_delay(i);

    else
      slice_free(tag_ptr);

  }

}


extern void* malloc(const unsigned long size) {

  unsigned int id;

  cc_executor_id(&id);

  return malloc_lf(size, id);

}


extern void free(void* const ptr) {

  unsigned int id;

  cc_executor_id(&id);

  return free_lf(ptr, id);

}


extern void* calloc(const unsigned long size, const unsigned long num) {

  void* const out = malloc(size * num);

  if(NULL != out)
    memset(out, 0, size * num);

  return out;

}


extern void* realloc(void* const ptr, const unsigned long size) {

  void* const out = malloc(size);

  if(NULL != out) {

    memcpy(out, ptr, size);
    free(ptr);

  }

  return out;

}
