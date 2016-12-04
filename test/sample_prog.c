/* Copyright (c) 2008 Eric McCorkle.  All rights reserved */

#include <stdint.h>

#include "gc_alloc.h"

/* This is a simple program which implements a Herlihy-style universal
 * construction.  Lots of threads access a B-tree with some nodes big
 * enough for array clustering, some not.  Regular objects are covered
 * by stack frames and tree nodes.  The program is a randomized stress
 * test, which just runs, causing the garbage collector to
 * periodically kick in.  If the program survives, then the test is
 * successful.
 *
 * This file will be compiled, and the assembly code modified.  Type
 * signatures and stack frames have to be created by hand.  The bulk
 * of the code (the B-tree implementation and random behavior code),
 * however, can be compiled, as long as the resulting functions are
 * modified to replace the stack allocation with allocation from the
 * GC-allocators.
 *
 * A normal IA-32 function call looks something like this:
 *
 * ...
 * push  eax ;; Argument 2
 * push  ebx ;; Argument 1
 * call  foo
 * add   esp,8
 * ...
 * foo:
 * sub   esp,8 ;; Make room for 2 locals
 * mov   ecx,[esp+4] ;; Assume ecx is callee-saved, and we use it.
 * ...               ;; Somewhere, [esp] gets used
 * add   esp,8
 * ret
 *
 * With this runtime, things are done differently.  In this example,
 * esi will hold the allocation pointer and edi will hold the limit.
 *
 * cmp   esi,edi+48 ;; Check if there's memory available
 * jg    .refresh_allocator ;; refresh the allocator if there's not
 * mov   [.header],xmm0 ;; initialize the object header
 * mov   xmm0,[esi]
 * mov   esp,[esp+4] ;; The old frame pointer needs to be saved
 * mov   esi,esp ;; complete allocation
 * add   esp,48
 * add   esp,16 ;; skip past the object header
 * mov   eax,[esp+8] ;; The arguments are saved to pre-initialized locals
 * mov   ebx,[esp+12]
 * call  foo
 * mov   esp,[esp+4] ;; re-load the old frame
 * foo:
 * mov   ecx,[esp+16] ;; just save the callee-saved register
 * ...   ;; and we're good to go
 *
 * A few words: I didn't optimize this code.  There are a number of
 * tricks that can be employed to reduce the function call overhead.
 * We also need to drop safe-points periodically.  The biggest change
 * is that most of the burden is now on the caller, not the callee.
 *
 * By using a page-fault trick, it may be possible to eliminate the
 * bounds check, though a decent CSE, flatten, register allocation,
 * and instruction scheduling pass ought to be able to reduce it to
 * negligible cost in practice.  Appel has a paper about this.
 */

/* Behavior thresholds.  Random numbers between one and 1000 are
 * generated, with each one causing a behavior.  The probabilities of
 * behaviors are changed at random, to make the system more chaotic.
 * There is always a 5% chance to change the probabilities.
 *
 * Possible behaviors are:
 * - spawn a thread
 * - die
 * - insert a node
 * - delete a node
 * - traverse the tree
 */

/* This will be updated Herlihy-style as well.  This tests non-pointer
 * data objects.
 */

typedef struct {

  unsigned int behavior_change = 50;
  unsigned int behavior_spawn;
  unsigned int behavior_die;
  unsigned int behavior_insert;
  unsigned int behavior_delete;

} behaviors_t;

static const unsigned int max_threads = 16384;

/* A mersenne twister RNG gives a good way to test long-lived arrays
 * and objects.  (Also, calling random() will require a switch to the
 * stack).
 *
 * Note: this also tests mutable objects.
 */
typedef struct {

  unsigned int mt_index;
  unsigned int _pad;
  /* All pointers are double-pointers, remember */
  uint32_t* mt_nums[2];

} mersenne_twister_t;

/* A thread's closure.  Contains the mersenne twister, a pointer to
 * the executor structure, and other information.
 */
typedef struct {

  /* not a heap pointer, so it's not a double-pointer */
  void* tc_mailbox;
  unsigned int _pad;
  mersenne_twister_t* tc_rng[2];

} thread_closure_t;

typedef struct btree_node_t btree_node_t;
typedef struct btree_content_t btree_content_t;

struct btree_content_t {

  unsigned int btc_num;
  unsigned int btc_isnull;
  btree_node_t* btc_child[2];

};

/* The tree is redundant, so it can be checked. */

struct btree_node_t {

  unsigned int btn_size;
  unsigned int btn_depth;
  btree_content_t* btn_content[2];

} btree_node_t;

/* Global state.  Both of these are double-pointers. */
static const behaviors_t* volatile behaviors[2];
static const btree_node_t* volatile tree[2];

static inline void safepoint(thread_closure_t* const restrict tc) {

}

/* We're initializing someone else's twister, remember... */
static inline void mt_init(thread_closure_t* const restrict tc,
			   mersenne_twister_t* const restrict rng,
			   const uint32_t seed) {

  rng->mt_index = 0;
  rng->mt_nums[0] = seed;
  /* XXX write-barriers */

  for(unsigned int i = 1; i < 624; i++) {

    if(!(i & 0xf))
      safepoint(tc);

    const uint32_t last = rng->mt_nums[i - 1];
    const uint32_t shift = last << 30;
    const uint32_t inner = last ^ shift;
    const uint32_t outer = inner * 0x6c078965;
    const uint32_t last = outer + i;

    new->tc_rng->mt_nums[i] = last & 0xffffffff;
    /* XXX write-barriers */

  }

}


static void mt_gen(thread_closure_t* const restrict tc) {

  for(unsigned int i = 0; i < 624; i++) {

    if(!(i & 0xf))
      safepoint(tc);

    const uint32_t y1 = (tc->tc_rng->mt_nums[i] & 0x80000000) +
      (tc->tc_rng->mt_nums[(i + 1) % 624] & 0x7ffffffff);
    const uint32_t y2 = tc->tc_rng->mt_nums[(i + 397) % 624] ^ (y1 >> 1);
    const uint32_t y3 = y2 & 0x1 ? y2 ^ 0x9908b0df : y2;

    tc->tc_rng_mt_nums[i] = y3;
    /* XXX write barrier */

  }

}


static uint32_t mt_random(thread_closure_t* const restrict tc) {

  const unsigned int index = tc->tc_rng->mt_index;

  if(0 == index)
    mt_gen(rng);

  uint32_t y1 = tc->tc_rng->mt_nums[index];
  uint32_t y2 = y1 ^ (y1 >> 11);
  uint32_t y3 = y2 ^ ((y2 << 6) & 0x9d2c5680);
  uint32_t y4 = y3 ^ ((y << 15) & 0xefc60000);
  uint32_t y5 = y4 ^ (y4 >> 18);

  tc->tc_rng->mt_index = (index + 1) % 624;
  /* XXX write-barrier */

  return y5;

}


static void change(mersenne_twister_t* const restrict rng) {

  const unsigned int spawn_num = mt_random(rng) % 1000;
  const unsigned int die_num = mt_random(rng) % 1000;
  const unsigned int insert_num = mt_random(rng) % 1000;
  const unsigned int delete_num = mt_random(rng) % 1000;
  const unsigned int traverse_num = mt_random(rng) % 1000;
  const unsigned int sum = spawn_num + die_num + insert_num +
    delete_num + traverse_num;

  behavior_spawn = 50 + ((spawn_num * 950) / sum);
  behavior_die = behavior_spawn + ((die_num * 950) / sum);
  behavior_insert = behavior_die + ((insert_num * 950) / sum);
  behavior_delete = behavior_insert + ((delete_num * 950) / sum);

}


static unsigned int do_traverse(thread_closure_t* const restrict tc,
				const btree_node_t* const restrict node,
				const unsigned int max) {

  if(NULL == node)

}


/* Traverse does a traversal of the tree, checking some invariants. */
static void traverse(thread_closure_t* const restrict tc) {

  const btree_node_t* const restrict root = /* Get the root */;

  if(NULL != root) {

    const unsigned int depth = root->btn_depth;
    const unsigned int size = root->btn_size;
    unsigned int subtree_depth = 0;
    unsigned int last = 0;

    for(unsigned int i = 0; i < size; i++) {

      const btree_content_t* const restrict content = root->btn_content + i;
      const btree_node_t* const restrict subtree = /* Get the subtree */;
      const unsigned int depth = do_traverse(tc, subtree);

      if(depth > subtree_depth)
	subtree_depth = depth;

      last = content->

	}

    if(subtree_depth + 1 != depth) {

    fprintf(stderr, "B-Tree was corrupted!  Depth was wrong.\n");
    abort();

  }

}


static void thread(thread_closure_t* const restrict tc) {

  for(unsigned int i = 0;; i = (i + 1) & 0xf) {

    if(!i)
      safepoint(tc);

    const unsigned int num = mt_random(tc) % 1000;

    if(num < behavior_change)
      change(tc);

    else if(num < behavior_spawn)
      spawn(tc);

    else if(num < behavior_die)
      die(tc);

    else if(num < behavior_insert)
      insert(tc);

    else if(num < behavior_delete)
      delete(tc);

    else
      traverse(tc);

  }

}
