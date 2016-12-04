/* Copyright (c) 2007, 2008 Eric McCorkle.  All rights reserved. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "definitions.h"
#include "atomic.h"
#include "cc/lf_thread_queue.h"
#include "cc/executor.h"

/* This is all taken from Maged Michael's paper on hazard pointers,
 * and Michael and Scott's paper on lock-free queues.  As this is all
 * code from research papers, it is NOT meant to be modified, and as
 * such, is not commented...
 */

typedef struct {

  thread_t* o_thread;
  bool o_valid;

} thread_node_opt_t;


/* This is a node used in a hash table used to efficiently detect
 * which values are protected by hazard pointers.  These hash-tables
 * are created entirely on the stack.
 */
typedef struct thread_ptab_node_t {

  struct thread_ptab_node_t* pn_next;
  lf_thread_queue_node_t* pn_addr;

} thread_ptab_node_t;


static unsigned int lf_thread_queue_hptrs;
static unsigned int lf_thread_queue_hptr_threshold;


internal pure unsigned int lf_thread_queue_request(unsigned int num,
						   unsigned int execs) {
  /* There might be a case where every executor has a full rlist and a
   * reference to a node, and the queue is full.
   */
  PRINTD("      Reserving space for thread queue\n");


  const unsigned int extra_nodes = (execs * execs * 2) + (execs * 2) + 1;
  const unsigned int nodes_size =
    sizeof(lf_thread_queue_node_t) * (extra_nodes + num);
  const unsigned int aligned_nodes_size =
    ((nodes_size - 1) & ~(CACHE_LINE_SIZE - 1)) + CACHE_LINE_SIZE;
  const unsigned int queue_size =
    sizeof(lf_thread_queue_t) + ((sizeof(lf_thread_queue_hazard_ptrs_t) +
				  (2 * sizeof(atomic_ptr_t))) * execs);
  const unsigned int aligned_queue_size =
    ((queue_size - 1) & ~(CACHE_LINE_SIZE - 1)) + CACHE_LINE_SIZE;
  const unsigned int allocator_size = sizeof(lf_thread_queue_node_allocator_t) +
    (execs * sizeof(lf_thread_queue_node_list_t));

  PRINTD("        Reserving 0x%x bytes for thread queue nodes.\n",
	 nodes_size);
  PRINTD("        Reserving 0x%x bytes for thread queue.\n",
	 queue_size);
  PRINTD("        Reserving 0x%x bytes for allocator.\n",
	 allocator_size);
  PRINTD("      Thread queue total static size is 0x%x bytes.\n",
	 aligned_queue_size + aligned_nodes_size + allocator_size);

  return aligned_queue_size + aligned_nodes_size + allocator_size;

}


internal lf_thread_queue_t* lf_thread_queue_init(void* const mem,
						 const unsigned int num,
						 const unsigned int execs) {

  INVARIANT(mem != NULL);
  INVARIANT(num > 0);

  const unsigned int extra_nodes = (execs * execs * 2) + (execs * 2) + 1;
  const unsigned int executor_nodes = (num + extra_nodes - 1) / execs;
  const unsigned int shared_nodes =
    (num + extra_nodes - 1) - (executor_nodes * execs);
  const unsigned int queue_size =
    sizeof(lf_thread_queue_t) + ((sizeof(lf_thread_queue_hazard_ptrs_t) +
				  (2 * sizeof(atomic_ptr_t))) * execs);
  const unsigned int aligned_queue_size =
    ((queue_size - 1) & ~(CACHE_LINE_SIZE - 1)) + CACHE_LINE_SIZE;
  const unsigned int allocator_size = sizeof(lf_thread_queue_node_allocator_t) +
    (execs * sizeof(lf_thread_queue_node_list_t));
  lf_thread_queue_t* fifo = mem;
  lf_thread_queue_node_allocator_t* allocator =
    (lf_thread_queue_node_allocator_t*)((char*)fifo + aligned_queue_size);
  lf_thread_queue_node_t* nodes =
    (lf_thread_queue_node_t*)((char*)allocator + allocator_size);
  lf_thread_queue_node_t* const sentinel =
    nodes + (num + extra_nodes - 1);
  const unsigned int nodes_size =
    sizeof(lf_thread_queue_node_t) * (extra_nodes + num);
  const unsigned int aligned_nodes_size =
    ((nodes_size - 1) & ~(CACHE_LINE_SIZE - 1)) + CACHE_LINE_SIZE;
  void* const out = (char*)nodes + aligned_nodes_size;

  PRINTD("Thread queue memory:\n");
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
  lf_thread_queue_hptrs = execs;
  lf_thread_queue_hptr_threshold = execs * 2;
  sentinel->lfn_next.value = NULL;
  sentinel->lfn_data = NULL;
  fifo->lf_head.value = sentinel;
  fifo->lf_tail.value = sentinel;
  fifo->lf_allocator = allocator;
  memset(fifo->lf_hptrs, 0, sizeof(lf_thread_queue_hazard_ptrs_t) * execs);
  PRINTD("Lock-free queue initialized\n");

  return out;

}


static inline unsigned int
lf_thread_queue_allocator_upper_bound(const lf_thread_queue_node_allocator_t*
				      const restrict alloc) {

  const unsigned int num_executors = executor_count();
  const unsigned int num_threads = alloc->qna_count;
  const unsigned int frac = num_threads >> 2;
  const unsigned int numerator = num_threads + frac;
  const unsigned int bound = numerator / num_executors;

  return bound != 0 ? bound : 1;

}


static inline unsigned int
lf_thread_queue_allocator_lower_bound(const lf_thread_queue_node_allocator_t*
				      const restrict alloc) {

  const unsigned int num_executors = executor_count();
  const unsigned int num_threads = alloc->qna_count;
  const unsigned int frac = num_threads >> 2;
  const unsigned int numerator = frac < num_threads ?
    num_threads - frac : 0;
  const unsigned int bound = numerator / num_executors;

  return bound != 0 ? bound : 1;

}


static inline bool
lf_thread_queue_node_alloc_try_push(lf_thread_queue_node_allocator_t*
				    const restrict alloc,
				    lf_thread_queue_node_t*
				    const restrict node) {

  lf_thread_queue_node_t* const head = alloc->qna_shared_list;

  node->lfn_next.value = head;

  return atomic_compare_and_set_ptr(head, node, &(alloc->qna_shared_list));

}


/* Push the node onto the shared stack */
static inline void
lf_thread_queue_node_alloc_push(lf_thread_queue_node_allocator_t*
				const restrict alloc,
				lf_thread_queue_node_t* const restrict node) {

  for(unsigned int i = 1;
      !lf_thread_queue_node_alloc_try_push(alloc, node);
      i++)
    backoff_delay(i);

}


static inline lf_thread_queue_node_t*
lf_thread_queue_node_alloc_try_pull(lf_thread_queue_node_allocator_t*
				    const restrict alloc) {

  lf_thread_queue_node_t* const head = alloc->qna_shared_list;
  lf_thread_queue_node_t* out;

  if(atomic_compare_and_set_ptr(head, head->lfn_next.value,
				&(alloc->qna_shared_list)))
    out = head;

  else
    out = NULL;

  return out;

}


/* Pull a node off the shared stack */
static inline lf_thread_queue_node_t*
lf_thread_queue_node_alloc_pull(lf_thread_queue_node_allocator_t*
				const restrict alloc) {

  lf_thread_queue_node_t* out;

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
lf_thread_queue_node_alloc_balance(lf_thread_queue_node_allocator_t*
				   const restrict alloc,
				   const unsigned int exec) {

  if(lf_thread_queue_allocator_upper_bound(alloc) <
     alloc->qna_lists[exec].nl_count) {

    lf_thread_queue_node_t* const node = alloc->qna_lists[exec].nl_list;

    lf_thread_queue_node_alloc_push(alloc, node);
    alloc->qna_lists[exec].nl_list = node->lfn_next.value;

  }

  else if(lf_thread_queue_allocator_lower_bound(alloc) >
	  alloc->qna_lists[exec].nl_count) {

    lf_thread_queue_node_t* const node = 
      lf_thread_queue_node_alloc_pull(alloc);

    node->lfn_next.value = alloc->qna_lists[exec].nl_list;
    alloc->qna_lists[exec].nl_list = node;

  }

}


/* Allocate a static node */
static inline lf_thread_queue_node_t*
lf_thread_queue_node_alloc(lf_thread_queue_t* const restrict fifo,
			   const unsigned int exec) {

  lf_thread_queue_node_allocator_t* const restrict alloc = fifo->lf_allocator;
  lf_thread_queue_node_t* out;

  lf_thread_queue_node_alloc_balance(alloc, exec);

  if(NULL != (out = alloc->qna_lists[exec].nl_list)) {

    alloc->qna_lists[exec].nl_list = out->lfn_next.value;
    alloc->qna_lists[exec].nl_count--;

  }

  return out;

}


/* Free a static node */
static inline void
lf_thread_queue_node_free(lf_thread_queue_t* const restrict fifo,
			  lf_thread_queue_node_t* const restrict node,
			  const unsigned int exec) {

  lf_thread_queue_node_allocator_t* const restrict alloc = fifo->lf_allocator;

  if(lf_thread_queue_allocator_lower_bound(alloc) <
     alloc->qna_lists[exec].nl_count) {

    node->lfn_next.value = alloc->qna_lists[exec].nl_list;
    alloc->qna_lists[exec].nl_list = node;
    alloc->qna_lists[exec].nl_count++;

  }

  else
    lf_thread_queue_node_alloc_push(alloc, node);

  lf_thread_queue_node_alloc_balance(alloc, exec);

}


static inline bool
lf_thread_queue_ptab_lookup(const thread_ptab_node_t* const * const
			    restrict ptab,
			    const lf_thread_queue_node_t* const node,
			    const unsigned int size) {

  INVARIANT(ptab != NULL);
  INVARIANT(node != NULL);

  const unsigned int addr = (unsigned int)node;
  const unsigned int index = (addr >> (CACHE_LINE_SIZE / 8)) % size;
  const thread_ptab_node_t* curr = ptab[index];
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
lf_thread_queue_ptab_insert(thread_ptab_node_t** const restrict ptab,
			    thread_ptab_node_t* const node,
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
lf_thread_queue_scan_build_ptab(lf_thread_queue_hazard_ptrs_t* const hptrs,
				thread_ptab_node_t nodes[][2],
				thread_ptab_node_t** const ptab) {

  const unsigned int size = (lf_thread_queue_hptrs * 4) - 1;

  memset(nodes, 0, sizeof(thread_ptab_node_t) * lf_thread_queue_hptrs * 2);
  memset(ptab, 0, size * sizeof(thread_ptab_node_t*));

  for(unsigned int i = 0; i < lf_thread_queue_hptrs; i++)
    for(unsigned int j = 0; j < 2; j++) {

	nodes[i][j].pn_addr = hptrs[i].thp_ptrs[j].value;
	lf_thread_queue_ptab_insert(ptab, nodes[i] + j, size);

    }

}


static inline void
lf_thread_queue_scan_free_rlist(lf_thread_queue_t* const fifo,
				const thread_ptab_node_t* const * const ptab,
				const unsigned int exec) {

  lf_thread_queue_hazard_ptrs_t* const hptrs = fifo->lf_hptrs + exec;
  const unsigned int size = (lf_thread_queue_hptrs * 4) - 1;
  lf_thread_queue_node_t* curr = hptrs->thp_rlist;

  /* This is OK, rlist and rcount are thread-local. */
  hptrs->thp_rlist = NULL;
  hptrs->thp_rcount = 0;

  while(NULL != curr) {

    lf_thread_queue_node_t* const next = curr->lfn_next.value;

    if(lf_thread_queue_ptab_lookup(ptab, curr, size)) {

      lf_thread_queue_node_t* const rlist = hptrs->thp_rlist;

      curr->lfn_next.value = rlist;
      hptrs->thp_rlist = curr;
      hptrs->thp_rcount++;

    }

    else
      lf_thread_queue_node_free(fifo, curr, exec);

    curr = next;

  }

}


static inline void lf_thread_queue_scan(lf_thread_queue_t* const restrict fifo,
					const unsigned int exec) {

  INVARIANT(fifo != NULL);

  const unsigned int size = (lf_thread_queue_hptrs * 4) - 1;
  thread_ptab_node_t nodes[lf_thread_queue_hptrs][2];
  thread_ptab_node_t* ptab[size];

  PRINTD("Executor %u scanning hazards\n", exec);
  PRINTD("Executor %u building table of hazards seen\n", exec);
  lf_thread_queue_scan_build_ptab(fifo->lf_hptrs, nodes, ptab);
  lf_thread_queue_scan_free_rlist(fifo, ptab, exec);

}


static inline
void lf_thread_queue_node_retire(lf_thread_queue_t* const restrict fifo,
				 lf_thread_queue_node_t* const restrict node,
				 const unsigned int exec) {

  INVARIANT(fifo != NULL);
  INVARIANT(node != fifo->lf_hptrs[exec].thp_rlist);
  INVARIANT(node != NULL);

  /* This is OK, rlist is thread-local. */
  lf_thread_queue_node_t* const rlist = fifo->lf_hptrs[exec].thp_rlist;

  PRINTD("Executor %u retiring %p\n", exec, node);
  PRINTD("%p->next = %p\n", node, rlist);
  PRINTD("rlist = %p\n", node);
  node->lfn_next.value = rlist;
  fifo->lf_hptrs[exec].thp_rlist = node;

  if(fifo->lf_hptrs[exec].thp_rcount++ >= lf_thread_queue_hptr_threshold)
    lf_thread_queue_scan(fifo, exec);

}


internal void lf_thread_queue_destroy(unused lf_thread_queue_t*
				      const restrict fifo,
				      unused const unsigned int exec) {

  INVARIANT(fifo != NULL);
  INVARIANT(fifo->lf_head.value != NULL);
  INVARIANT(fifo->lf_tail.value != NULL);
  INVARIANT(((lf_thread_queue_node_t*)
	     fifo->lf_head.value)->lfn_next.value == NULL);

  /* There is no need to do anything, since there is no dynamically
   * allocated memery here.
   */
  PRINTD("Executor %u destroying lock-free queue %p\n", exec, fifo);

}


static inline lf_thread_queue_node_t*
lf_thread_queue_try_enqueue(lf_thread_queue_t* const restrict fifo,
			    lf_thread_queue_node_t* const restrict node,
			    const unsigned int exec) {

  INVARIANT(fifo != NULL);
  INVARIANT(node != NULL);

  lf_thread_queue_node_t* const tail = fifo->lf_tail.value;
  lf_thread_queue_node_t* out = NULL;

  PRINTD("Tail is %p\n", tail);
  fifo->lf_hptrs[exec].thp_ptrs[0].value = tail;

  if(tail == fifo->lf_tail.value) {

    lf_thread_queue_node_t* const next = tail->lfn_next.value;

    PRINTD("Next is %p\n", next);

    if(tail == fifo->lf_tail.value) {

      if(NULL != next) {

	PRINTD("Tail pointer changed.  Retrying.\n");
	atomic_compare_and_set_ptr(tail, next, &(fifo->lf_tail));

      }

      else if(atomic_compare_and_set_ptr(NULL, node, &(tail->lfn_next))) {

	PRINTD("Enqueue successful.\n");
	out = tail;

      }

    }

  }

  return out;

}


internal bool lf_thread_queue_enqueue(lf_thread_queue_t* const restrict fifo,
				      thread_t* const restrict thread,
				      const unsigned int exec) {

  INVARIANT(fifo != NULL);
  INVARIANT(thread != NULL);
  INVARIANT(thread->t_sched_stat_ref.value & T_REF);

  lf_thread_queue_node_t* const node = lf_thread_queue_node_alloc(fifo, exec);
  bool out;

  if(out = (NULL != node)) {

    lf_thread_queue_node_t* tail;

    PRINTD("Executor %u enqueueing thread %p queue %p in node %p\n",
	   exec, thread, fifo, node);
    node->lfn_data = thread;
    node->lfn_next.value = NULL;

    for(unsigned int i = 0;
	NULL == (tail = lf_thread_queue_try_enqueue(fifo, node, exec));
	i++)
      backoff_delay(i);

    PRINTD("Executor %u trying to set tail pointer from %p to %p.\n",
	   exec, tail, node);
    atomic_compare_and_set_ptr(tail, node, &(fifo->lf_tail));
    PRINTD("Executor %u unsetting hazard pointer\n", exec);
    fifo->lf_hptrs[exec].thp_ptrs[0].value = NULL;
    out = true;

  }

  return out;

}


static inline
thread_node_opt_t lf_thread_queue_try_dequeue(lf_thread_queue_t* const
					      restrict fifo,
					      const unsigned int exec) {

  INVARIANT(fifo != NULL);

  lf_thread_queue_node_t* const head = fifo->lf_head.value;
  thread_node_opt_t out = { .o_thread = NULL, .o_valid = false };

  PRINTD("Executor %u trying to dequeue from queue %p\n",
	 exec, fifo);
  fifo->lf_hptrs[exec].thp_ptrs[0].value = head;

  if(head == fifo->lf_head.value) {

    lf_thread_queue_node_t* const tail = fifo->lf_tail.value;
    lf_thread_queue_node_t* const next = head->lfn_next.value;

    fifo->lf_hptrs[exec].thp_ptrs[1].value = next;

    if(head == fifo->lf_head.value) {

      if(NULL != next) {

	if(head != tail) {

	  if(atomic_compare_and_set_ptr(head, next, &(fifo->lf_head))) {

	    out.o_thread = next->lfn_data;
	    fifo->lf_hptrs[exec].thp_ptrs[1].value = NULL;
	    lf_thread_queue_node_retire(fifo, head, exec);
	    fifo->lf_hptrs[exec].thp_ptrs[0].value = NULL;
	    out.o_valid = true;

	  }

	}

	else
	  atomic_compare_and_set_ptr(tail, next, &(fifo->lf_tail));

      }

      else {

	fifo->lf_hptrs[exec].thp_ptrs[0].value = NULL;
	fifo->lf_hptrs[exec].thp_ptrs[1].value = NULL;
	out.o_valid = true;

      }

    }

  }

  PRINTD(!out.o_valid ? "Executor %u failed\n" : "Executor %u succeeded\n",
	 exec);

  return out;

}


internal thread_t*
lf_thread_queue_dequeue(lf_thread_queue_t* const restrict fifo,
			const unsigned int exec) {

  INVARIANT(fifo != NULL);

  thread_node_opt_t res;

  PRINTD("Executor %u dequeueing from lock-free queue %p\n",
	 exec, fifo);

  for(unsigned int i = 0;
      !(res = lf_thread_queue_try_dequeue(fifo, exec)).o_valid;
      i++)
    backoff_delay(i);

  return res.o_thread;

}
