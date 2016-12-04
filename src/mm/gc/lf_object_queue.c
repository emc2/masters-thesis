/* Copyright (c) 2007, 2008 Eric McCorkle.  All rights reserved. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "definitions.h"
#include "atomic.h"
#include "mm/lf_object_queue.h"
#include "cc/executor.h"

/* This is all taken from Maged Michael's paper on hazard pointers,
 * and Michael and Scott's paper on lock-free queues.  As this is all
 * code from research papers, it is NOT meant to be modified, and as
 * such, is not commented...
 */

typedef struct {

  void* o_object;
  bool o_valid;

} object_node_opt_t;


/* This is a node used in a hash table used to efficiently detect
 * which values are protected by hazard pointers.  These hash-tables
 * are created entirely on the stack.
 */
typedef struct object_ptab_node_t {

  struct object_ptab_node_t* pn_next;
  lf_object_queue_node_t* pn_addr;

} object_ptab_node_t;


static unsigned int lf_object_queue_hptrs;
static unsigned int lf_object_queue_hptr_threshold;


internal pure unsigned int lf_object_queue_request(unsigned int num,
						  unsigned int execs) {
  /* There might be a case where every executor has a full rlist and a
   * reference to a node, and the queue is full.
   */
  PRINTD("    Reserving space for object queue\n");

  const unsigned int extra_nodes = (execs * execs * 2) + (execs * 2) + 1;
  const unsigned int nodes_size =
    sizeof(lf_object_queue_node_t) * (extra_nodes + num);
  const unsigned int aligned_nodes_size =
    ((nodes_size - 1) & ~(CACHE_LINE_SIZE - 1)) + CACHE_LINE_SIZE;
  const unsigned int queue_size =
    sizeof(lf_object_queue_t) + ((sizeof(lf_object_queue_hazard_ptrs_t) +
				  (2 * sizeof(atomic_ptr_t))) * execs);
  const unsigned int aligned_queue_size =
    ((queue_size - 1) & ~(CACHE_LINE_SIZE - 1)) + CACHE_LINE_SIZE;
  const unsigned int allocator_size = sizeof(lf_object_queue_node_allocator_t) +
    (execs * sizeof(lf_object_queue_node_list_t));

  PRINTD("      Reserving 0x%x bytes for object queue nodes.\n",
	 nodes_size);
  PRINTD("      Reserving 0x%x bytes for object queue.\n",
	 queue_size);
  PRINTD("      Reserving 0x%x bytes for allocator.\n",
	 allocator_size);
  PRINTD("    Object queue total static size is 0x%x bytes.\n",
	 aligned_queue_size + aligned_nodes_size + allocator_size);

  return aligned_queue_size + aligned_nodes_size + allocator_size;

}


internal lf_object_queue_t* lf_object_queue_init(void* const mem,
					       const unsigned int num,
					       const unsigned int execs) {

  INVARIANT(mem != NULL);
  INVARIANT(num > 0);

  PRINTD("Initializing object queue, static memory at 0x%p.\n", mem);

  const unsigned int extra_nodes = (execs * execs * 2) + (execs * 2) + 1;
  const unsigned int executor_nodes = (num + extra_nodes - 1) / execs;
  const unsigned int shared_nodes =
    (num + extra_nodes - 1) - (executor_nodes * execs);
  const unsigned int queue_size =
    sizeof(lf_object_queue_t) + ((sizeof(lf_object_queue_hazard_ptrs_t) +
				  (2 * sizeof(atomic_ptr_t))) * execs);
  const unsigned int aligned_queue_size =
    ((queue_size - 1) & ~(CACHE_LINE_SIZE - 1)) + CACHE_LINE_SIZE;
  const unsigned int allocator_size = sizeof(lf_object_queue_node_allocator_t) +
    (execs * sizeof(lf_object_queue_node_list_t));
  lf_object_queue_t* fifo = mem;
  lf_object_queue_node_allocator_t* allocator =
    (lf_object_queue_node_allocator_t*)((char*)fifo + aligned_queue_size);
  lf_object_queue_node_t* nodes =
    (lf_object_queue_node_t*)((char*)allocator + allocator_size);
  lf_object_queue_node_t* const sentinel =
    nodes + (num + extra_nodes - 1);
  const unsigned int nodes_size =
    sizeof(lf_object_queue_node_t) * (extra_nodes + num);
  const unsigned int aligned_nodes_size =
    ((nodes_size - 1) & ~(CACHE_LINE_SIZE - 1)) + CACHE_LINE_SIZE;
  void* const out = (char*)nodes + aligned_nodes_size;

  PRINTD("Object queue memory:\n");
  PRINTD("\tobject at 0x%p\n", mem);
  PRINTD("\tallocator at 0x%p\n", allocator);
  PRINTD("\tnodes at 0x%p\n", nodes);
  PRINTD("\tend at 0x%p\n", out);

  /* Set up all the lists and nodes. */
  for(unsigned int i = 0; i < execs; i++) {

    nodes[i * executor_nodes].lfn_next.value = NULL;
    allocator->qna_lists[i].nl_count = executor_nodes;
    allocator->qna_lists[i].nl_list =
      nodes + (i * executor_nodes) + (executor_nodes - 1);

    for(unsigned int j = 1; j < executor_nodes; j++)
      nodes[(i * executor_nodes) + j].lfn_next.value =
	nodes + (i * executor_nodes) + j - 1;

  }

  /* Turn the extra nodes into shared nodes */
  if(0 == shared_nodes)
    allocator->qna_shared_list = NULL;

  else {

    allocator->qna_shared_list = nodes + (num + extra_nodes - 1);
    nodes[executor_nodes * execs].lfn_next.value = NULL;

    for(unsigned int i = (executor_nodes * execs) + 1;
	i < num + extra_nodes - 1; i++)
      nodes[i].lfn_next.value = nodes + (i - 1);

  }

  allocator->qna_count = num + extra_nodes;
  PRINTD("Initializing lock-free queue %p for %u nodes and %u executors\n",
	 fifo, num, execs);
  PRINTD("Initial dummy node is %p\n", sentinel);
  lf_object_queue_hptrs = execs;
  lf_object_queue_hptr_threshold = execs * 2;
  sentinel->lfn_next.value = NULL;
  sentinel->lfn_data = NULL;
  fifo->lf_head.value = sentinel;
  fifo->lf_tail.value = sentinel;
  fifo->lf_allocator = allocator;
  memset(fifo->lf_hptrs, 0, sizeof(lf_object_queue_hazard_ptrs_t) * execs);
  PRINTD("Lock-free queue initialized\n");

  return out;

}


static inline unsigned int
lf_object_queue_allocator_upper_bound(const lf_object_queue_node_allocator_t*
				      const restrict alloc) {

  const unsigned int num_executors = executor_count();
  const unsigned int num_objects = alloc->qna_count;
  const unsigned int frac = num_objects >> 2;
  const unsigned int numerator = num_objects + frac;
  const unsigned int bound = numerator / num_executors;

  return bound != 0 ? bound : 1;

}


static inline unsigned int
lf_object_queue_allocator_lower_bound(const lf_object_queue_node_allocator_t*
				      const restrict alloc) {

  const unsigned int num_executors = executor_count();
  const unsigned int num_objects = alloc->qna_count;
  const unsigned int frac = num_objects >> 2;
  const unsigned int numerator = frac < num_objects ?
    num_objects - frac : 0;
  const unsigned int bound = numerator / num_executors;

  return bound != 0 ? bound : 1;

}


static inline bool
lf_object_queue_node_alloc_try_push(lf_object_queue_node_allocator_t*
				    const restrict alloc,
				    lf_object_queue_node_t*
				    const restrict node) {

  lf_object_queue_node_t* const head = alloc->qna_shared_list;

  node->lfn_next.value = head;

  return atomic_compare_and_set_ptr(head, node, &(alloc->qna_shared_list));

}


/* Push the node onto the shared stack */
static inline void
lf_object_queue_node_alloc_push(lf_object_queue_node_allocator_t*
				const restrict alloc,
				lf_object_queue_node_t* const restrict node) {

  for(unsigned int i = 1;
      !lf_object_queue_node_alloc_try_push(alloc, node);
      i++)
    backoff_delay(i);

}


static inline lf_object_queue_node_t*
lf_object_queue_node_alloc_try_pull(lf_object_queue_node_allocator_t*
				    const restrict alloc) {

  lf_object_queue_node_t* const head = alloc->qna_shared_list;
  lf_object_queue_node_t* out;

  if(atomic_compare_and_set_ptr(head, head->lfn_next.value,
				&(alloc->qna_shared_list)))
    out = head;

  else
    out = NULL;

  return out;

}


/* Pull a node off the shared stack */
static inline lf_object_queue_node_t*
lf_object_queue_node_alloc_pull(lf_object_queue_node_allocator_t*
				const restrict alloc) {

  lf_object_queue_node_t* out;

  for(unsigned int i = 1;; i++) {

    out = alloc->qna_shared_list;

    if(NULL != out) {

      if(atomic_compare_and_set_ptr(out, out->lfn_next.value,
				    &(alloc->qna_shared_list)))
	break;

      else
	backoff_delay(i);

    }

    else
      break;

  }

  return out;

}


/* Balance nodes with the shared list */
static inline void
lf_object_queue_node_alloc_balance(lf_object_queue_node_allocator_t*
				   const restrict alloc,
				   const unsigned int exec) {

  if(lf_object_queue_allocator_upper_bound(alloc) <
     alloc->qna_lists[exec].nl_count) {

    lf_object_queue_node_t* const node = alloc->qna_lists[exec].nl_list;

    lf_object_queue_node_alloc_push(alloc, node);
    alloc->qna_lists[exec].nl_list = node->lfn_next.value;

  }

  else if(lf_object_queue_allocator_lower_bound(alloc) >
	  alloc->qna_lists[exec].nl_count) {

    lf_object_queue_node_t* const node = 
      lf_object_queue_node_alloc_pull(alloc);

    node->lfn_next.value = alloc->qna_lists[exec].nl_list;
    alloc->qna_lists[exec].nl_list = node;

  }

}


/* Allocate a static node */
static inline lf_object_queue_node_t*
lf_object_queue_node_alloc(lf_object_queue_t* const restrict fifo,
			   const unsigned int exec) {

  lf_object_queue_node_allocator_t* const restrict alloc = fifo->lf_allocator;
  lf_object_queue_node_t* out;

  lf_object_queue_node_alloc_balance(alloc, exec);

  if(NULL != (out = alloc->qna_lists[exec].nl_list)) {

    alloc->qna_lists[exec].nl_list = out->lfn_next.value;
    alloc->qna_lists[exec].nl_count--;

  }

  return out;

}


/* Free a static node */
static inline void
lf_object_queue_node_free(lf_object_queue_t* const restrict fifo,
			  lf_object_queue_node_t* const restrict node,
			  const unsigned int exec) {

  lf_object_queue_node_allocator_t* const restrict alloc = fifo->lf_allocator;

  if(lf_object_queue_allocator_lower_bound(alloc) <
     alloc->qna_lists[exec].nl_count) {

    node->lfn_next.value = alloc->qna_lists[exec].nl_list;
    alloc->qna_lists[exec].nl_list = node;
    alloc->qna_lists[exec].nl_count++;

  }

  else
    lf_object_queue_node_alloc_push(alloc, node);

  lf_object_queue_node_alloc_balance(alloc, exec);

}


static inline bool
lf_object_queue_ptab_lookup(const object_ptab_node_t* const * const
			    restrict ptab,
			    const lf_object_queue_node_t* const node,
			    const unsigned int size) {

  INVARIANT(ptab != NULL);
  INVARIANT(node != NULL);

  const unsigned int addr = (unsigned int)node;
  const unsigned int index = (addr >> (CACHE_LINE_SIZE / 8)) % size;
  const object_ptab_node_t* curr = ptab[index];
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
lf_object_queue_ptab_insert(object_ptab_node_t** const restrict ptab,
			    object_ptab_node_t* const node,
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


static inline void
lf_object_queue_scan_build_ptab(lf_object_queue_hazard_ptrs_t* const hptrs,
				object_ptab_node_t nodes[][2],
				object_ptab_node_t** const ptab) {

  const unsigned int size = (lf_object_queue_hptrs * 4) - 1;

  memset(nodes, 0, sizeof(object_ptab_node_t) * lf_object_queue_hptrs * 2);
  memset(ptab, 0, size * sizeof(object_ptab_node_t*));

  for(unsigned int i = 0; i < lf_object_queue_hptrs; i++)
    for(unsigned int j = 0; j < 2; j++) {

	nodes[i][j].pn_addr = hptrs[i].ohp_ptrs[j].value;
	lf_object_queue_ptab_insert(ptab, nodes[i] + j, size);

    }

}


static inline void
lf_object_queue_scan_free_rlist(lf_object_queue_t* const fifo,
				const object_ptab_node_t* const * const ptab,
				const unsigned int exec) {

  lf_object_queue_hazard_ptrs_t* const hptrs = fifo->lf_hptrs + exec;
  const unsigned int size = (lf_object_queue_hptrs * 4) - 1;
  lf_object_queue_node_t* curr = hptrs->ohp_rlist;

  /* This is OK, rlist and rcount are thread-local. */
  hptrs->ohp_rlist = NULL;
  hptrs->ohp_rcount = 0;

  while(NULL != curr) {

    lf_object_queue_node_t* const next = curr->lfn_next.value;

    if(lf_object_queue_ptab_lookup(ptab, curr, size)) {

      lf_object_queue_node_t* const rlist = hptrs->ohp_rlist;

      curr->lfn_next.value = rlist;
      hptrs->ohp_rlist = curr;
      hptrs->ohp_rcount++;

    }

    else
      lf_object_queue_node_free(fifo, curr, exec);

    curr = next;

  }

}


static inline void lf_object_queue_scan(lf_object_queue_t* const restrict fifo,
					const unsigned int exec) {

  INVARIANT(fifo != NULL);

  const unsigned int size = (lf_object_queue_hptrs * 4) - 1;
  object_ptab_node_t nodes[lf_object_queue_hptrs][2];
  object_ptab_node_t* ptab[size];

  PRINTD("Executor %u scanning hazards\n", exec);
  PRINTD("Executor %u building table of hazards seen\n", exec);
  lf_object_queue_scan_build_ptab(fifo->lf_hptrs, nodes, ptab);
  lf_object_queue_scan_free_rlist(fifo, ptab, exec);

}


static inline
void lf_object_queue_node_retire(lf_object_queue_t* const restrict fifo,
				 lf_object_queue_node_t* const restrict node,
				 const unsigned int exec) {

  INVARIANT(fifo != NULL);
  INVARIANT(node != fifo->lf_hptrs[exec].ohp_rlist);
  INVARIANT(node != NULL);

  /* This is OK, rlist is thread-local. */
  lf_object_queue_node_t* const rlist = fifo->lf_hptrs[exec].ohp_rlist;

  PRINTD("Executor %u retiring %p\n", exec, node);
  PRINTD("%p->next = %p\n", node, rlist);
  PRINTD("rlist = %p\n", node);
  node->lfn_next.value = rlist;
  fifo->lf_hptrs[exec].ohp_rlist = node;

  if(fifo->lf_hptrs[exec].ohp_rcount++ >= lf_object_queue_hptr_threshold)
    lf_object_queue_scan(fifo, exec);

}


internal void lf_object_queue_destroy(unused lf_object_queue_t*
				     const restrict fifo,
				     unused const unsigned int exec) {

  INVARIANT(fifo != NULL);
  INVARIANT(fifo->lf_head.value != NULL);
  INVARIANT(fifo->lf_tail.value != NULL);
  INVARIANT(((lf_object_queue_node_t*)
	     fifo->lf_head.value)->lfn_next.value == NULL);

  /* There is no need to do anything, since there is no dynamically
   * allocated memery here.
   */
  PRINTD("Executor %u destroying lock-free queue %p\n", exec, fifo);

}


static inline lf_object_queue_node_t*
lf_object_queue_try_enqueue(lf_object_queue_t* const restrict fifo,
			   lf_object_queue_node_t* const restrict node,
			   const unsigned int exec) {

  INVARIANT(fifo != NULL);
  INVARIANT(node != NULL);

  lf_object_queue_node_t* const tail = fifo->lf_tail.value;
  lf_object_queue_node_t* out = NULL;

  PRINTD("Tail is %p\n", tail);
  fifo->lf_hptrs[exec].ohp_ptrs[0].value = tail;

  if(tail == fifo->lf_tail.value) {

    lf_object_queue_node_t* const next = tail->lfn_next.value;

    PRINTD("Next is %p\n", next);

    if(tail == fifo->lf_tail.value) {

      if(NULL != next)
	atomic_compare_and_set_ptr(tail, next, &(fifo->lf_tail));


      else if(atomic_compare_and_set_ptr(NULL, node, &(tail->lfn_next)))
	out = tail;

    }

  }

  return out;

}


internal bool lf_object_queue_enqueue(lf_object_queue_t* const restrict fifo,
				     void* const restrict object,
				     const unsigned int exec) {

  INVARIANT(fifo != NULL);
  INVARIANT(object != NULL);

  lf_object_queue_node_t* const node = lf_object_queue_node_alloc(fifo, exec);
  bool out;

  if(out = (NULL != node)) {

    lf_object_queue_node_t* tail;

    PRINTD("Executor %u enqueueing object %p queue %p in node %p\n",
	   exec, object, fifo, node);
    node->lfn_data = object;
    node->lfn_next.value = NULL;

    for(unsigned int i = 0;
	NULL == (tail = lf_object_queue_try_enqueue(fifo, node, exec));
	i++)
      backoff_delay(i);

    PRINTD("Executor %u trying to set tail pointer from %p to %p.\n",
	   exec, tail, node);
    atomic_compare_and_set_ptr(tail, node, &(fifo->lf_tail));
    PRINTD("Executor %u unsetting hazard pointer\n", exec);
    fifo->lf_hptrs[exec].ohp_ptrs[0].value = NULL;
    out = true;

  }

  return out;

}


static inline
object_node_opt_t lf_object_queue_try_dequeue(lf_object_queue_t* const
					      restrict fifo,
					      const unsigned int exec) {

  INVARIANT(fifo != NULL);

  lf_object_queue_node_t* const head = fifo->lf_head.value;
  object_node_opt_t out = { .o_object = NULL, .o_valid = false };

  PRINTD("Executor %u trying to dequeue from queue %p\n",
	 exec, fifo);
  fifo->lf_hptrs[exec].ohp_ptrs[0].value = head;

  if(head == fifo->lf_head.value) {

    lf_object_queue_node_t* const tail = fifo->lf_tail.value;
    lf_object_queue_node_t* const next = head->lfn_next.value;

    fifo->lf_hptrs[exec].ohp_ptrs[1].value = next;

    if(head == fifo->lf_head.value) {

      if(NULL != next) {

	if(head != tail) {

	  if(atomic_compare_and_set_ptr(head, next, &(fifo->lf_head))) {

	    out.o_object = next->lfn_data;
	    fifo->lf_hptrs[exec].ohp_ptrs[1].value = NULL;
	    lf_object_queue_node_retire(fifo, head, exec);
	    fifo->lf_hptrs[exec].ohp_ptrs[0].value = NULL;
	    out.o_valid = true;

	  }

	}

	else
	  atomic_compare_and_set_ptr(tail, next, &(fifo->lf_tail));

      }

      else {

	fifo->lf_hptrs[exec].ohp_ptrs[0].value = NULL;
	fifo->lf_hptrs[exec].ohp_ptrs[1].value = NULL;
	out.o_valid = true;

      }

    }

  }

  PRINTD(!out.o_valid ? "Executor %u failed\n" : "Executor %u succeeded\n",
	 exec);

  return out;

}


internal void* lf_object_queue_dequeue(lf_object_queue_t* const restrict fifo,
				      const unsigned int exec) {

  INVARIANT(fifo != NULL);

  object_node_opt_t res;

  PRINTD("Executor %u dequeueing from lock-free queue %p\n",
	 exec, fifo);

  for(unsigned int i = 0;
      !(res = lf_object_queue_try_dequeue(fifo, exec)).o_valid;
      i++)
    backoff_delay(i);

  return res.o_object;

}
