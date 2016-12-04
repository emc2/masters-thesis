#include <stdio.h>
#include <stdlib.h>
#include "definitions.h"
#include "cc.h"
#include "program.h"
#include "atomic.h"
#include "../src/arch/atomic.c"

#define THREADS 2

const unsigned int default_cc_num_executors = THREADS;
const unsigned int default_cc_executor_stack_size = 0;
const unsigned int default_cc_max_threads = 0;
const unsigned int default_mm_total_limit = 0;
const unsigned int default_mm_malloc_limit = 0;
const unsigned int default_mm_gc_limit = 0;
const unsigned int default_mm_slice_size = 0x400000;
const unsigned int default_gc_gens = 3;
const unsigned int default_gc_array_gen = 2;
gc_double_ptr_t* const gc_global_ptrs[0] = {};
const unsigned int gc_global_ptr_count = 0;

struct block_t {

  unsigned int size;
  unsigned char content;
  unsigned char space[];

};

static thread_t threads[THREADS];
static unsigned char stacks[THREADS][0x8000];
static atomic_ptr_t blocks[0x4000];
static void* const claimed = (void*)(-1);
static unsigned int contexts[THREADS];
static atomic_uint_t behaviors;


static unsigned int get_random(unsigned int id) {

  unsigned int out = rand_r(contexts + id);

  out <<= 8;
  out |= rand_r(contexts + id);
  out <<= 8;
  out |= rand_r(contexts + id);
  out <<= 8;
  out |= rand_r(contexts + id);

  return out;

}


static unsigned int select_process(unsigned int id) {

  const unsigned int behaviors_value = behaviors.value;
  const unsigned int check_gap = behaviors_value & 0xff;
  const unsigned int update_gap = (behaviors_value >> 8) & 0xff;
  const unsigned int delete_gap = (behaviors_value >> 16) & 0xff;
  const unsigned int change_gap = 4;
  const unsigned int check_limit = check_gap;
  const unsigned int update_limit = check_limit + update_gap;
  const unsigned int delete_limit = update_limit + delete_gap;
  const unsigned int limit = delete_limit + change_gap;
  const unsigned int value = get_random(id) % limit;
  unsigned int out;

  PRINTD("Check %u(%u), update %u(%u), delete %u(%u), change %u(%u)\n",
	 check_limit, check_gap, update_limit, update_gap,
	 delete_limit, delete_gap, limit, change_gap);

  if(value < check_limit)
    out = 0;

  else if(value < update_limit)
    out = 1;

  else if(value < delete_limit)
    out = 2;

  else
    out = 3;

  return out;

}


static void change(unsigned int id) {

  const unsigned int new = get_random(id);

  PRINTD("Changing behaviors to %u\n", new);

  behaviors.value = new;

}

static void check(struct block_t* block) {

  PRINTD("Checking block %p\n", block);

  for(unsigned int i = 0; i < block->size; i++)
    if(block->space[i] != block->content) {

      fprintf(stderr, "Block %p corrupted at index %x: "
	      "content was originally %x, but was %x\n", block, i,
	      block->content, block->space[i]);
      abort();

    }

}


static void update(struct block_t* block, unsigned int id) {

  const unsigned char new = get_random(id);

  PRINTD("Updating block %p\n", block);

  check(block);

  block->content = new;

  for(unsigned int i = 0; i < block->size; i++)
    block->space[i] = new;

}


static struct block_t* create(unsigned int id) {

  const unsigned int behaviors_value = behaviors.value;
  const unsigned int limit = (behaviors_value >> 24) & 0xff;
  const unsigned int value = get_random(id) % 256;
  struct block_t* out;

  if(value < limit) {

    PRINTD("Creating a new block\n");

    const unsigned int size = get_random(id) % 0x2000;
    const unsigned char content = get_random(id);

    PRINTD("Block will be size %x, containing %x\n", size, content);

    out = malloc(sizeof(struct block_t) + size);

    PRINTD("Block is located at %p\n", out);

    out->size = size;
    out->content = content;

    for(unsigned int i = 0; i < size; i++)
      out->space[i] = content;

  }

  else
    out = NULL;

  return out;

}


static void process(unsigned int index, unsigned int id) {

  struct block_t* orig;
  struct block_t* new;

  PRINTD("Processing block %u\n", index);

  do {

    orig = blocks[index].value;

  } while(claimed != orig &&
	  !atomic_compare_and_set_ptr(orig, claimed, blocks + index));

  store_fence();

  if(claimed != orig) {

    PRINTD("Block %u was unclaimed.\n", index);

    if(NULL ==  orig)
      new = create(id);

    else
      switch(select_process(id)) {

      case 0:

	check(orig);
	new = orig;

	break;

      case 1:

	update(orig, id);
	new = orig;

	break;

      case 2:

	PRINTD("Destroying block %p\n", orig);
	free(orig);
	new = NULL;

	break;

      default:

	change(id);

	break;

      }

    store_fence();
    blocks[index].value = new;

  }

  else
    PRINTD("Block %u was claimed\n", index);

}


static void thread_func(unsigned int id) {

  PRINTD("%d Running regular thread function\n", id);

  for(;;) {

    const unsigned int index = get_random(id) % 0x4000;

    process(index, id);

  }

}


noreturn void prog_main(thread_t* const restrict thread,
			const unsigned int exec,
			const unsigned int argc,
			const char* const * const argv,
			const char* const * const envp) {

  thread_stat_t stat;
  thread_mbox_t mbox;
  void (**retaddr_ptr)(void) = (void (**)(void))(&mbox);
  void** stkptr_ptr = (void**)(mbox + thread_mbox_stkptr);
  bool result;

  PRINTD("Starting main program.\n");

  memset(mbox, 0, sizeof(thread_mbox_t));
  *retaddr_ptr = thread_func;
  stat.t_id = 0;
  stat.t_pri = 0;
  stat.t_sched_stat = T_STAT_RUNNABLE;
  behaviors.value = 0x80808080;

  for(unsigned int i = 0; i < THREADS; i++) {

    unsigned int* stkptr_value = stacks[i] + 0x7fe0;
    volatile void* ptr;

    stkptr_value[0] = i;
    contexts[i] = random();
    *stkptr_ptr = stkptr_value;
    cc_thread_create(&stat, mbox, &ptr, threads + i, exec);

  }


  stat.t_sched_stat = T_STAT_TERM;
  cc_thread_update(thread, &stat, exec, &result);
  cc_safepoint(exec, 0x1);
  fprintf(stderr, "Executing a destroyed thread\n");
  exit(0);

}
