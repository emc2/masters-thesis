#include <stdio.h>

#include "definitions.h"
#include "atomic.h"
#include "cc.h"
#include "cc/thread.h"

#include "../src/arch/atomic.c"

#define THREADS 0x1000

const unsigned int default_cc_num_executors = 0;
const unsigned int default_cc_executor_stack_size = 0x40000;
const unsigned int default_cc_max_threads = THREADS;
const unsigned int default_mm_total_limit = 0;
const unsigned int default_mm_malloc_limit = 0;
const unsigned int default_mm_gc_limit = 0;
const unsigned int default_mm_slice_size = 0x400000;

struct thread_data_t {

  volatile atomic_ptr_t thread;
  volatile void* stkptr;
  struct thread_data_t* next;

};

static char stacks[THREADS][0x8000];
static struct thread_data_t threads[THREADS];
static thread_t* const claimed = (void*)(-1);

struct hazard_ptrs_t {

  unsigned int rcount;
  struct thread_data_t* rlist;
  volatile atomic_ptr_t ptr;

};


static volatile struct hazard_ptrs_t hptrs[2];
static volatile atomic_uint_t live_threads;

noreturn void run(volatile thread_mbox_t* mbox,
		  struct thread_data_t* tdata);

static inline bool lookup(const void* ptr) {

  bool out = false;

  for(unsigned int i = 0; i < 2; i++)
    if(out = (hptrs[i].ptr.value == ptr))
      break;

  return out;

}


void do_cc_thread_destroy(thread_t* const restrict thread,
			  const unsigned int exec,
			  void* const restrict stack);

static inline void scan(unsigned int exec, void* const restrict stack) {

  struct thread_data_t* curr = hptrs[exec].rlist;

  /* This is OK, rlist and rcount are thread-local. */
  hptrs[exec].rlist = NULL;
  hptrs[exec].rcount = 0;

  while(NULL != curr) {

    struct thread_data_t* const next = curr->next;

    if(lookup(curr)) {

      struct thread_data_t* const rlist = hptrs[exec].rlist;

      curr->next = rlist;
      hptrs[exec].rlist = curr;
      hptrs[exec].rcount++;

    }

    else {

      do_cc_thread_destroy(curr, exec, stack);

    }

    curr = next;

  }

}


void do_cc_thread_create(const thread_stat_t* const restrict stat,
			 const thread_mbox_t mbox,
			 const unsigned int exec,
			 volatile void* volatile * const mbox_addr_ptr,
			 thread_t** const restrict thread,
			 void* const restrict stack);


noreturn void do_cc_sched_cycle(const unsigned int exec,
				void* const restrict stack);


void do_cc_thread_update_immediate(thread_t* const restrict thread,
				   const thread_stat_t* const restrict stat,
				   const unsigned int exec,
				   bool* const restrict result,
				   void* const restrict stack);


static inline void thread_retire(thread_t* thread, unsigned int exec,
				 void* const restrict stack) {

  /* This is OK, rlist is thread-local */
  thread_t* const rlist = hptrs[exec].rlist;

  thread->t_rlist_next = rlist;
  hptrs[exec].rlist = thread;

  if(hptrs[exec].rcount++ >= 4)
    scan(exec, stack);

}


static void create(volatile struct thread_data_t* restrict tdata,
		   unsigned int exec, void* const restrict stack) {

  thread_stat_t stat;
  thread_mbox_t mbox;
  thread_t* thread;
  volatile void* volatile * const aligned_stkptr =
    (volatile void* volatile *)((char*)(tdata->stkptr) + 0x7fe0);
  void (* volatile * const retaddr_ptr)(void) =
    (void (* volatile *)(void))((char*)mbox + thread_mbox_retaddr);
  volatile unsigned int* const executor_ptr =
    (volatile unsigned int*)((char*)mbox + thread_mbox_executor);
  volatile void* volatile * const stkptr_ptr =
    (volatile void* volatile *)((char*)mbox + thread_mbox_stkptr);
  volatile atomic_uint_t* volatile * const sigptr_ptr =
    (volatile atomic_uint_t* volatile *)((char*)mbox + thread_mbox_sigptr);

  *retaddr_ptr = (void (*)(void))run;
  *stkptr_ptr = aligned_stkptr;
  *executor_ptr = 0;
  *sigptr_ptr = NULL;
  stat.t_sched_stat = random() & 0x1 ? T_STAT_RUNNABLE : T_STAT_SUSPEND;
  stat.t_destroy = NULL;
  PRINTD("Mailbox pointer %p\n", aligned_stkptr);
  do_cc_thread_create(&stat, mbox, exec, aligned_stkptr, &thread, stack);
  PRINTD("Mailbox pointer %p\n", aligned_stkptr[0]);
  tdata->thread.value = thread;
  atomic_increment_uint(&live_threads);

}


static inline void destroy(volatile struct thread_data_t* restrict tdata,
			   thread_t* restrict thread,
			   unsigned int exec, void* const restrict stack) {

  PRINTD("Executor %u destroying thread at %p\n", exec, tdata);

  if(atomic_compare_and_set_ptr(thread, NULL, &(tdata->thread))) {

    thread_stat_t stat;
    bool val;

    stat.t_sched_stat = T_STAT_TERM;
    do_cc_thread_update_immediate(thread, &stat, exec, &val, stack);
    thread_retire(thread, exec, stack);

  }

  else
    atomic_increment_uint(&live_threads);

}


static inline void toggle(thread_t* restrict thread, unsigned int exec,
			  void* const restrict stack) {

  const thread_sched_stat_t sched_stat =
    (thread_sched_stat_t)(thread->t_sched_stat_ref.value & T_STAT_MASK);
  thread_stat_t stat;
  const unsigned int live = live_threads.value;
  bool val;

  PRINTD("Executor %u toggling thread %p\n", exec, thread);
  //  fprintf(stderr, "There are %u live threads\n", live_threads.value);

  if(T_STAT_RUNNING == sched_stat || T_STAT_RUNNABLE == sched_stat) {

    if(live > 1 && atomic_compare_and_set_uint(live, live - 1, &live_threads)) {

      stat.t_sched_stat = T_STAT_SUSPEND;
      do_cc_thread_update_immediate(thread, &stat, exec, &val, stack);

    }

  }

  else if(T_STAT_SUSPEND == sched_stat || T_STAT_SUSPENDED == sched_stat) {

    stat.t_sched_stat = T_STAT_RUNNABLE;
    do_cc_thread_update_immediate(thread, &stat, exec, &val, stack);
    atomic_increment_uint(&live_threads);

  }

}


static inline void alter(unsigned int exec, void* const restrict stack) {

  volatile struct thread_data_t* const tdata = threads + (random() & 0xfff);
  thread_t* const thread = tdata->thread.value;
  const unsigned int live = live_threads.value;

  PRINTD("Executor %u modifying thread at %p\n", exec, tdata);
  hptrs[exec].ptr.value = thread;

  if(thread == tdata->thread.value) {

    if(NULL == thread) {

      if(atomic_compare_and_set_ptr(NULL, claimed, &(tdata->thread)))
	create(tdata, exec, stack);

    }

    else if(claimed != thread) {

      if(random() & 0x1 && live > 1 &&
	 atomic_compare_and_set_uint(live, live - 1, &live_threads))
	destroy(tdata, thread, exec, stack);

      else
	toggle(thread, exec, stack);

    }

  }

  hptrs[exec].ptr.value = NULL;

}


noreturn void run(volatile thread_mbox_t* mbox,
		  struct thread_data_t* tdata) {

  volatile void* volatile * const aligned_stkptr =
    (volatile void* volatile *)((char*)(tdata->stkptr) + 0x7fe0);
  void (* volatile * const retaddr_ptr)(void) =
    (void (*volatile *)(void))((char*)mbox + thread_mbox_retaddr);
  volatile void* volatile * const stkptr_ptr =
    (volatile void* volatile *)((char*)mbox + thread_mbox_stkptr);
  volatile unsigned int* const executor_ptr =
    (volatile unsigned int*)((char*)mbox + thread_mbox_executor);
  const unsigned int executor = *executor_ptr;
  void* const restrict runtime_stack = *(void**)stkptr_ptr;

  PRINTD("Executor %d entering run function\n", executor);
  PRINTD("Runtime stack is %p\n", runtime_stack);
  *retaddr_ptr = (void (*)(void))run;
  *stkptr_ptr = aligned_stkptr;

  for(unsigned int i = 0; i < 10; i++)
    alter(executor, runtime_stack);

  do_cc_sched_cycle(executor, runtime_stack);

}


static void create_initial(volatile struct thread_data_t* restrict tdata,
			   thread_t* restrict thread,
			   unsigned int exec) {

  volatile void* volatile * const aligned_stkptr =
    (volatile void* volatile *)((char*)(tdata->stkptr) + 0x7fe0);
  void (* volatile * const retaddr_ptr)(void) =
    (void (* volatile *)(void))((char*)thread->t_mbox + thread_mbox_retaddr);
  volatile void* volatile * const stkptr_ptr =
    (volatile void* volatile *)((char*)thread->t_mbox + thread_mbox_stkptr);

  *retaddr_ptr = (void (*)(void))run;
  *stkptr_ptr = aligned_stkptr;
  aligned_stkptr[0] = &(thread->t_mbox);
  aligned_stkptr[1] = tdata;
  tdata->thread.value = thread;

}


noreturn void entry(volatile void* stkptr, thread_t* thread, unsigned int exec,
		    unused unsigned int argc, unused const char* const * argv,
		    unused const char* const * envp) {


  PRINTD("Initializing test program\n");
  threads[0].thread.value = thread;
  threads[0].stkptr = stacks[0];
  live_threads.value = 1;

  for(unsigned int i = 1; i < THREADS; i++) {

    volatile void* volatile * aligned_stkptr;

    threads[i].stkptr = stacks[i];
    aligned_stkptr = (volatile void* volatile *)
      ((char*)(threads[i].stkptr) + 0x7fe0);
    aligned_stkptr[1] = threads + i;

    if(NULL != threads[i].stkptr)
      threads[i].thread.value = NULL;

    else {

      perror("Error in runtime: Cannot allocate memory");
      abort();

    }

  }

  create_initial(threads, thread, exec);
  PRINTD("Initialization complete\n");
  cc_sched_cycle(exec);

}
