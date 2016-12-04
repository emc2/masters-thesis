/* Copyright (c) 2007 Eric McCorkle.  All rights reserved. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "definitions.h"
#include "atomic.h"
#include "mm/slice.h"
#include "mm/lf_block_queue.h"

/* This is all taken from Maged Michael's paper on hazard pointers,
 * and Michael and Scott's paper on lock-free queues.  As this is all
 * code from research papers, it is NOT meant to be modified, and as
 * such, is not commented...
 */

typedef struct {

  void* o_block;
  bool o_valid;

} malloc_node_opt_t;


typedef struct malloc_ptab_node_t {

  struct malloc_ptab_node_t* pn_next;
  lf_block_queue_node_t* pn_addr;

} malloc_ptab_node_t;

static unsigned int lf_block_queue_hptrs;
static unsigned int lf_block_queue_hptr_threshold;

static volatile atomic_ptr_t lf_block_queue_nodes;

static inline malloc_node_opt_t lf_block_queue_try_alloc_node(void) {

  lf_block_queue_node_t* const node = lf_block_queue_nodes.value;
  malloc_node_opt_t out = { .o_block = node, .o_valid = false };

  if(NULL != node) {

    lf_block_queue_node_t* const next = node->lfn_next.value;

    out.o_valid =
      atomic_compare_and_set_ptr(node, next, &lf_block_queue_nodes);

  }

  else {

    /* XXX do a two-level slice allocation */
    slice_t* const slice = slice_alloc(SLICE_TYPE_MALLOC,
				      SLICE_PROT_RWX, 0x10000);

    if(slice != NULL) {

      lf_block_queue_node_t* const ptr = slice->s_ptr;
      const unsigned int limit = slice->s_size / sizeof(lf_block_queue_node_t);

      for(unsigned int i = 1; i < limit; i++)
	ptr[i].lfn_next.value = ptr + (i + 1);

      ptr[limit - 1].lfn_next.value = NULL;
      store_fence();

      if(out.o_valid = atomic_compare_and_set_ptr(NULL, ptr + 1,
						  &lf_block_queue_nodes))
	out.o_block = ptr;

      else
	slice_free(slice);

    }

    else
      out.o_valid = true;

  }

  return out;

}


static inline lf_block_queue_node_t* lf_block_queue_alloc_node(void) {

  malloc_node_opt_t out;

  for(unsigned int i = 0;
      !(out = lf_block_queue_try_alloc_node()).o_valid;
      i++)
    backoff_delay(i);

  return out.o_block;

}


internal void lf_block_queue_init(lf_block_queue_t* const restrict fifo,
				  const unsigned int execs) {

  INVARIANT(fifo != NULL);
  INVARIANT(execs > 0);

  lf_block_queue_node_t* const sentinel = lf_block_queue_alloc_node();

  if(NULL != sentinel) {

    PRINTD("Initializing block queue, static memory at 0x%p.\n", fifo);
    PRINTD("Initial sentinel node is %p\n", sentinel);
    lf_block_queue_hptrs = execs;
    lf_block_queue_hptr_threshold = execs * 2;
    sentinel->lfn_next.value = NULL;
    sentinel->lfn_data = NULL;
    fifo->lf_head.value = sentinel;
    fifo->lf_tail.value = sentinel;
    memset(fifo->lf_hptrs, 0, sizeof(lf_block_queue_hazard_ptrs_t) * execs);

  }

  else {

    panic("Error in runtime: Cannot allocate memory");

  }

}


static inline bool
lf_block_queue_ptab_lookup(const malloc_ptab_node_t* const * const
			   restrict ptab,
			   const lf_block_queue_node_t* const node,
			   const unsigned int size) {

  INVARIANT(ptab != NULL);
  INVARIANT(node != NULL);

  const unsigned int addr = (unsigned int)node;
  const unsigned int index = (addr >> (CACHE_LINE_SIZE / 8)) % size;
  const malloc_ptab_node_t* curr = ptab[index];
  bool out = false;

  PRINTD("Looking up %p in ptab, index %u, next %p\n",
	 node, index, curr);

  while(NULL != curr && !out) {

    out = node == curr->pn_addr;
    curr = curr->pn_next;

  }

  return out;

}


static inline void
lf_block_queue_ptab_insert(malloc_ptab_node_t** const restrict ptab,
			   malloc_ptab_node_t* const node,
			   const unsigned int size) {

  INVARIANT(ptab != NULL);
  INVARIANT(node != NULL);

  const unsigned int addr = (unsigned int)(node->pn_addr);
  const unsigned int index = (addr >> (CACHE_LINE_SIZE / 8)) % size;

  PRINTD("Inserting %p into ptab, index %u, next %p\n",
	 node->pn_addr, index, ptab[index]);

  node->pn_next = ptab[index];
  ptab[index] = node;

}


/* Bulk-free the entire unreferenced rlist */

static inline bool
lf_block_queue_try_free_nodes(lf_block_queue_node_t* const free_head,
			      lf_block_queue_node_t* const free_tail) {

  lf_block_queue_node_t* const list = lf_block_queue_nodes.value;

  free_tail->lfn_next.value = list;

  store_fence();

  return atomic_compare_and_set_ptr(list, free_head, &lf_block_queue_nodes);

}


static inline void
lf_block_queue_scan_free_rlist(lf_block_queue_hazard_ptrs_t* const hptrs,
			       const malloc_ptab_node_t* const * const ptab) {

  const unsigned int size = (lf_block_queue_hptrs * 4) - 1;
  lf_block_queue_node_t* curr = hptrs->bhp_rlist;
  lf_block_queue_node_t* free_head = NULL;
  lf_block_queue_node_t* free_tail = NULL;

  /* This is OK, rlist and rcount are thread-local. */
  hptrs->bhp_rlist = NULL;
  hptrs->bhp_rcount = 0;

  while(NULL != curr) {

    lf_block_queue_node_t* const next = curr->lfn_next.value;

    if(lf_block_queue_ptab_lookup(ptab, curr, size)) {

      lf_block_queue_node_t* const rlist = hptrs->bhp_rlist;

      curr->lfn_next.value = rlist;
      hptrs->bhp_rlist = curr;
      hptrs->bhp_rcount++;

    }

    else {

      if(NULL == free_head) {

	free_head = curr;
	free_tail = curr;

      }

      else {

	free_tail->lfn_next.value = curr;
	free_tail = curr;

      }

    }

    curr = next;

  }

  for(unsigned int i = 0;
      !lf_block_queue_try_free_nodes(free_head, free_tail);
      i++)
    backoff_delay(i);

}


static inline void
lf_block_queue_scan_build_ptab(lf_block_queue_hazard_ptrs_t* const hptrs,
			       malloc_ptab_node_t nodes[][2],
			       malloc_ptab_node_t** const ptab) {

  const unsigned int size = (lf_block_queue_hptrs * 4) - 1;

  memset(nodes, 0, sizeof(malloc_ptab_node_t) * lf_block_queue_hptrs * 2);
  memset(ptab, 0, size * sizeof(malloc_ptab_node_t*));

  for(unsigned int i = 0; i < lf_block_queue_hptrs; i++)
    for(unsigned int j = 0; j < 2; j++) {

      nodes[i][j].pn_addr = hptrs[i].bhp_ptrs[j].value;
      lf_block_queue_ptab_insert(ptab, nodes[i] + j, size);

    }

}


static inline void lf_block_queue_scan(lf_block_queue_t* const restrict fifo,
				       const unsigned int exec) {

  INVARIANT(fifo != NULL);

  const unsigned int size = (lf_block_queue_hptrs * 4) - 1;
  malloc_ptab_node_t nodes[lf_block_queue_hptrs][2];
  malloc_ptab_node_t* ptab[size];

  PRINTD("Executor %u scanning hazards\n", exec);
  PRINTD("Executor %u building table of hazards seen\n", exec);
  lf_block_queue_scan_build_ptab(fifo->lf_hptrs, nodes, ptab);
  lf_block_queue_scan_free_rlist(fifo->lf_hptrs + exec, ptab);

}


static inline
void lf_block_queue_node_retire(lf_block_queue_t* const restrict fifo,
				lf_block_queue_node_t* const restrict node,
				const unsigned int exec) {

  INVARIANT(fifo != NULL);
  INVARIANT(node != fifo->lf_hptrs[exec].bhp_rlist);
  INVARIANT(node != NULL);

  /* This is OK, rlist is thread-local. */
  lf_block_queue_node_t* const rlist = fifo->lf_hptrs[exec].bhp_rlist;

  PRINTD("Executor %u retiring %p\n", exec, node);
  PRINTD("%p->next = %p\n", node, rlist);
  PRINTD("rlist = %p\n", node);
  node->lfn_next.value = rlist;
  fifo->lf_hptrs[exec].bhp_rlist = node;

  if(fifo->lf_hptrs[exec].bhp_rcount++ >= lf_block_queue_hptr_threshold)
    lf_block_queue_scan(fifo, exec);

}


/*!
 * This function destroys the fifo passed in and reclaims its resources.
 *
 * \brief Destroy a fifo.
 * \arg fifo The fifo to be destroyed.
 * \arg exec The ID of the calling executor.
 */
internal void lf_block_queue_destroy(lf_block_queue_t* const restrict fifo,
				     const unsigned int exec) {

  INVARIANT(fifo != NULL);
  INVARIANT(fifo->lf_head.value != NULL);
  INVARIANT(fifo->lf_tail.value != NULL);
  //INVARIANT(fifo->lf_head.value == fifo->lf_tail.value);
  INVARIANT(((lf_block_queue_node_t*)
	     fifo->lf_head.value)->lfn_next.value == NULL);

  PRINTD("Executor %u destroying lock-free queue %p\n", exec, fifo);
  lf_block_queue_node_retire(fifo, fifo->lf_head.value, exec);
  lf_block_queue_scan(fifo, exec);

}


static inline lf_block_queue_node_t*
lf_block_queue_try_enqueue(lf_block_queue_t* const restrict fifo,
			   lf_block_queue_node_t* const restrict node,
			   const unsigned int exec) {

  INVARIANT(fifo != NULL);
  INVARIANT(node != NULL);

  lf_block_queue_node_t* const tail = fifo->lf_tail.value;
  lf_block_queue_node_t* out = NULL;

  PRINTD("Executor %u trying to enqueue %p into queue %p\n",
	 exec, node, fifo);
  PRINTD("Tail is %p\n", tail);
  fifo->lf_hptrs[exec].bhp_ptrs[0].value = tail;

  if(tail == fifo->lf_tail.value) {

    lf_block_queue_node_t* const next = tail->lfn_next.value;

    PRINTD("Next is %p\n", next);

    if(tail == fifo->lf_tail.value) {

      if(NULL != next) {

	PRINTD("Executor %u advancing tail during enqueue on %p\n",
	       exec, fifo);
	atomic_compare_and_set_ptr(tail, next, &(fifo->lf_tail));

      }

      else if(atomic_compare_and_set_ptr(NULL, node, &(tail->lfn_next)))
	out = tail;

    }

  }

  PRINTD(NULL == out ? "Executor %u failed\n" : "Executor %u succeeded\n",
	 exec);

  return out;

}


/*!
 * This function enqueues block onto fifo.  This function operates
 * atomically on the fifo, and can be called by any number of threads.
 *
 * \brief Enqueue a block.
 * \arg fifo The fifo onto which to enqueue.
 * \arg exec The ID of the calling executor.
 * \arg block The block to enqueue.
 */
internal void lf_block_queue_enqueue(lf_block_queue_t* const restrict fifo,
				     void* const restrict block,
				     const unsigned int exec) {

  INVARIANT(fifo != NULL);
  INVARIANT(block != NULL);

  lf_block_queue_node_t* const node = lf_block_queue_alloc_node();
  lf_block_queue_node_t* tail;

  if(NULL != node) {

    PRINTD("Executor %u enqueueing block %p queue %p in node %p\n",
	   exec, block, fifo, node);
    node->lfn_data = block;
    node->lfn_next.value = NULL;

    for(unsigned int i = 0;
	NULL == (tail = lf_block_queue_try_enqueue(fifo, node, exec));
	i++)
      backoff_delay(i);

    PRINTD("Executor %u trying to set tail pointer from %p to %p.\n",
	   exec, tail, node);
    atomic_compare_and_set_ptr(tail, node, &(fifo->lf_tail));
    PRINTD("Executor %u unsetting hazard pointer\n", exec);
    fifo->lf_hptrs[exec].bhp_ptrs[0].value = NULL;

  }

  else {

    panic("Error in runtime: Cannot allocate memory");

  }

}


static inline
malloc_node_opt_t lf_block_queue_try_dequeue(lf_block_queue_t* const
					     restrict fifo,
					     const unsigned int exec) {

  INVARIANT(fifo != NULL);

  lf_block_queue_node_t* const head = fifo->lf_head.value;
  malloc_node_opt_t out = { .o_block = NULL, .o_valid = false };

  PRINTD("Executor %u trying to dequeue from queue %p\n",
	 exec, fifo);
  fifo->lf_hptrs[exec].bhp_ptrs[0].value = head;

  if(head == fifo->lf_head.value) {

    lf_block_queue_node_t* const tail = fifo->lf_tail.value;
    lf_block_queue_node_t* const next = head->lfn_next.value;

    fifo->lf_hptrs[exec].bhp_ptrs[1].value = next;

    if(head == fifo->lf_head.value) {

      if(NULL != next) {

	if(head != tail) {

	  if(atomic_compare_and_set_ptr(head, next, &(fifo->lf_head))) {

	    out.o_block = next->lfn_data;
	    fifo->lf_hptrs[exec].bhp_ptrs[1].value = NULL;
	    lf_block_queue_node_retire(fifo, head, exec);
	    fifo->lf_hptrs[exec].bhp_ptrs[0].value = NULL;
	    out.o_valid = true;

	  }

	}

	else
	  atomic_compare_and_set_ptr(tail, next, &(fifo->lf_tail));

      }

      else {

	fifo->lf_hptrs[exec].bhp_ptrs[0].value = NULL;
	fifo->lf_hptrs[exec].bhp_ptrs[1].value = NULL;
	out.o_valid = true;

      }

    }

  }

  PRINTD(!out.o_valid ? "Executor %u failed\n" : "Executor %u succeeded\n",
	 exec);

  return out;

}


/*!
 * This function dequeues a thread from fifo and returns it.  This
 * function operates atomically on fifo.
 *
 * \brief Dequeue an item from the thread queue.
 * \arg fifo The fifo from which to dequeue.
 * \arg exec The ID of the calling executor.
 * \return The dequeued thread.
 */
internal void*
lf_block_queue_dequeue(lf_block_queue_t* const restrict fifo,
		       const unsigned int exec) {

  INVARIANT(fifo != NULL);

  malloc_node_opt_t res;

  PRINTD("Executor %u dequeueing from lock-free queue %p\n",
	 exec, fifo);

  for(unsigned int i = 0;
      !(res = lf_block_queue_try_dequeue(fifo, exec)).o_valid;
      i++)
    backoff_delay(i);

  return res.o_block;

}
