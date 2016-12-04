#include <stdio.h>
#include <stdlib.h>
#include "definitions.h"
#include "cc.h"
#include "program.h"
#include "atomic.h"
#include "../src/arch/atomic.c"

#define MIN_SLICE_POWER 14
#define MAX_SLICE_POWER BITS - 1
#define THREADS 2

typedef struct thread_closure_t {

  volatile unsigned int tc_frame[16];
  thread_t tc_thread;
  unsigned int tc_rand_context;
  unsigned int tc_id_gen;

  unsigned int tc_exec;
  volatile gc_allocator_t* tc_allocators;
  unsigned int tc_log_offset;
  volatile gc_log_entry_t* tc_log;
  unsigned int tc_gc_state;

} thread_closue_t;

static const gc_typedesc_t graph_node_type =
  { GC_TYPEDESC_CONST | GC_TYPEDESC_NORMAL, 2 * sizeof(unsigned int), 1, 0 };
static const gc_typedesc_t ptr_array_type =
  { GC_TYPEDESC_ARRAY, 0, 1, 0 };
static const gc_typedesc_t list_node_type =
  { GC_TYPEDESC_NORMAL, sizeof(unsigned int), 1, 2 };
static const gc_typedesc_t id_array_type =
  { GC_TYPEDESC_ARRAY, sizeof(unsigned int), 0, 0 };
static const gc_typedesc_t frame_type =
  { GC_TYPEDESC_NORMAL, 0, 4, 0 };
static void* volatile entry[2];
static void* volatile list[2];
static unsigned char stacks[THREADS][0x8000];
static void* const claimed = (void*)(-1);
static atomic_uint_t behaviors;
static atomic_uint_t num_nodes;
const unsigned int default_cc_num_executors = THREADS;
const unsigned int default_cc_executor_stack_size = 0;
const unsigned int default_cc_max_threads = 0;
const unsigned int default_mm_total_limit = 0;
const unsigned int default_mm_malloc_limit = 0;
const unsigned int default_mm_gc_limit = 0;
const unsigned int default_mm_slice_size = 0x400000;
const unsigned int default_gc_gens = 3;
const unsigned int default_gc_array_gen = 2;
const unsigned int gc_global_ptr_count = 2;
gc_double_ptr_t* const gc_global_ptrs[0] = { entry, list };


static unsigned int get_random(thread_closure_t* const closure) {

  unsigned int out = rand_r(&(closure->tc_rand_context));

  out <<= 8;
  out |= rand_r(&(closure->tc_rand_context));
  out <<= 8;
  out |= rand_r(&(closure->tc_rand_context));
  out <<= 8;
  out |= rand_r(&(closure->tc_rand_context));

  return out;

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


static void refresh_alloc(thread_closure_t* const closure,
			  const unsigned int size,
			  const unsigned int gen) {

  do_gc_allocator_refresh(closure->tc_allocators, size,
			  size, gen, closure->tc_exec);

}


static void run_gc(thread_closure_t* const closure) {

}


static inline void safe_point(thread_closure_t* const closure) {

  closure->tc_gc_state = gc_state.value;

}


static inline void* gc_allocator_alloc(thread_closure_t* const closure,
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
  else
    out = refresh_alloc(closure, size, gen);

  return out;

}


static inline void write_barrier(thread_closure_t* const closure,
				 void* const header,
				 const unsigned int offset) {

  const unsigned int state = gc_state.value & GC_STATE_PHASE;

  if(GC_STATE_INACTIVE != state && GC_STATE_INITIAL != state) {

    PRINTD("Executor %u executing write barrier, GC is active.\n", exec);

    const unsigned int use_index = (closure->tc_log_offset)++;
    void* const log_entry = closure->tc_log + use_index;
    void* volatile * const header_ptr = (void* volatile *)log_entry;
    volatile unsigned int* const offset_ptr =
      (volatile unsigned int*)((void* volatile *)log_entry + 1);

    *header_ptr = header;
    *offset_ptr = offset;

    if(GC_WRITE_LOG_LENGTH == *index)
      run_gc(closure);

  }

  else
    PRINTD("Executor %u executing write barrier, but GC was not active.\n",
	   exec);

}


static void alloc_graph_node(thread_closure_t* const closure,
			     const unsigned int id,
			     const unsigned int arr_size,
			     void* const arr) {

  const bool flipflop = gc_collection_count & 0x1;
  const unsigned int used_ptr = flipflop ? 0 : 1;
  const unsigned int unused_ptr = flipflop ? 1 : 0;
  void* const unclaimed = flipflop ? (void*)0 : (void*)~0;
  const unsigned int size = sizeof(gc_header_t) +
    (2 * sizeof(unsigned int)) + (2 * sizeof(void*));
  void* const header = gc_allocator_alloc(closure->tc_allocators[1], size, 0);
  void* volatile * const fwd_ptr_ptr = (void* volatile *)header;
  void** const list_ptr_ptr = (void**)header + 1;
  unsigned int* const typeinfo_ptr = (unsigned int*)header + 2;
  unsigned int* const geninfo_ptr = (unsigned int*)header + 3;
  void* const data_ptr = (char*)header + sizeof(gc_header_t);
  unsigned int* const id_ptr = (unsigned int*)data_ptr;
  unsigned int* const size_ptr = (unsigned int*)data_ptr + 1;
  void** const arr_ptr = (void**)((unsigned int*)data_ptr + 2);
  void** frame_ptr = (void**)(closure->tc_frame + 4);

  *fwd_ptr_ptr = unclaimed;
  *list_ptr_ptr = NULL;
  *typeinfo_ptr = graph_node_type;
  *geninfo_ptr = 0;
  *id_ptr = id;
  *size_ptr = arr_size;
  arr_ptr[used_ptr] = arr;
  arr_ptr[unused_ptr] = NULL;
  frame_ptr[used_ptr] = ptr;
  write_barrier(closure, closure->tc_frame, 4 * sizeof(void**));

}


static void alloc_ptr_array(thread_closure_t* const closure,
			    const unsigned int size) {

  const bool flipflop = gc_collection_count & 0x1;
  const unsigned int used_ptr = flipflop ? 0 : 1;
  const unsigned int unused_ptr = flipflop ? 1 : 0;
  void* const unclaimed = flipflop ? (void*)0 : (void*)~0;
  const unsigned int size = sizeof(gc_header_t) + (2 * size * sizeof(void*));
  void* const header = gc_allocator_alloc(closure->tc_allocators[2], size, 0);
  void* volatile * const fwd_ptr_ptr = (void* volatile *)header;
  void** const list_ptr_ptr = (void**)header + 1;
  unsigned int* const typeinfo_ptr = (unsigned int*)header + 2;
  unsigned int* const geninfo_ptr = (unsigned int*)header + 3;
  void* const data_ptr = (char*)header + sizeof(gc_header_t);
  void** frame_ptr = (void**)(closure->tc_frame + 6);

  *fwd_ptr_ptr = unclaimed;
  *list_ptr_ptr = NULL;
  *typeinfo_ptr = ptr_array_type;
  *geninfo_ptr = 0;
  memset(data_ptr, 0, 2 * size * sizeof(void*));
  frame_ptr[used_ptr] = ptr;
  write_barrier(closure, closure->tc_frame, 6 * sizeof(void**));

}


static void alloc_list_node(thread_closure_t* const closure,
			    const unsigned int id,
			    void* const node,
			    void* const arr) {

  const bool flipflop = gc_collection_count & 0x1;
  const unsigned int used_ptr = flipflop ? 0 : 1;
  const unsigned int unused_ptr = flipflop ? 1 : 0;
  void* const unclaimed = flipflop ? (void*)0 : (void*)~0;
  const unsigned int size = sizeof(gc_header_t) +
    sizeof(unsigned int) + (6 * sizeof(void*));
  void* const header = gc_allocator_alloc(closure->tc_allocators[1], size, 0);
  void* volatile * const fwd_ptr_ptr = (void* volatile *)header;
  void** const list_ptr_ptr = (void**)header + 1;
  unsigned int* const typeinfo_ptr = (unsigned int*)header + 2;
  unsigned int* const geninfo_ptr = (unsigned int*)header + 3;
  void* const data_ptr = (char*)header + sizeof(gc_header_t);
  unsigned int* const id_ptr = (unsigned int*)data_ptr;
  void** const next_ptr = (void**)((unsigned int*)data_ptr + 1);
  void** const node_ptr = next_ptr + 2;
  void** const arr_ptr = node_ptr + 2;
  void** frame_ptr = (void**)(closure->tc_frame + 8);

  *fwd_ptr_ptr = unclaimed;
  *list_ptr_ptr = NULL;
  *typeinfo_ptr = list_node_type;
  *geninfo_ptr = 0;
  *id_ptr = id;
  node_ptr[used_ptr] = node;
  node_ptr[unused_ptr] = NULL;
  arr_ptr[used_ptr] = node;
  arr_ptr[unused_ptr] = NULL;
  frame_ptr[used_ptr] = ptr;
  write_barrier(closure, closure->tc_frame, 8 * sizeof(void**));

}


static void alloc_id_array(thread_closure_t* const closure,
			   const unsigned int size) {

  const bool flipflop = gc_collection_count & 0x1;
  const unsigned int used_ptr = flipflop ? 0 : 1;
  const unsigned int unused_ptr = flipflop ? 1 : 0;
  void* const unclaimed = flipflop ? (void*)0 : (void*)~0;
  const unsigned int size = sizeof(gc_header_t) + (size * sizeof(void*));
  void* const header = gc_allocator_alloc(closure->tc_allocators[2], size, 0);
  void* volatile * const fwd_ptr_ptr = (void* volatile *)header;
  void** const list_ptr_ptr = (void**)header + 1;
  unsigned int* const typeinfo_ptr = (unsigned int*)header + 2;
  unsigned int* const geninfo_ptr = (unsigned int*)header + 3;
  void* const data_ptr = (char*)header + sizeof(gc_header_t);
  void** frame_ptr = (void**)(closure->tc_frame + 10);

  *fwd_ptr_ptr = unclaimed;
  *list_ptr_ptr = NULL;
  *typeinfo_ptr = id_array_type;
  *geninfo_ptr = 0x00000101;
  memset(data_ptr, 0, size * sizeof(void*));
  frame_ptr[used_ptr] = ptr;
  write_barrier(closure, closure->tc_frame, 10 * sizeof(void**));

}


static bool try_insert_node(thread_closure_t* const closure) {

  const bool flipflop = gc_collection_count & 0x1;
  const unsigned int used_ptr = flipflop ? 0 : 1;
  void* const list_value = list[used_ptr];
  void* volatile * const list_node_ptr =
    (void* volatile *)(closure->tc_frame + 8);
  void* const new_node = list_node_ptr[used_ptr];
  void* const data_ptr = (char*)new_node + sizeof(gc_header_t);
  void** const next_ptr = (void**)((unsigned int*)data_ptr + 1);
  bool out;

  next_ptr[used_ptr] = list_value;
  write_barrier(closure, new_node, sizeof(unsigned int));

  if(out = atomic_compare_and_set_ptr(list_value, new_node, list + used_ptr))
    /* write barrier on the list */;

  return out;

}


static void create(thread_closure_t* const closure) {

  const unsigned int oldid = closure->tc_id_gen++;
  const unsigned int newid = 0 == oldid ? closure->tc_id_gen++ : oldid;
  const unsigned int size = random() % 0x4000;
  void* volatile * const graph_node_ptr =
    (void* volatile *)(closure->tc_frame + 4);
  void* volatile * const ptr_array_ptr =
    (void* volatile *)(closure->tc_frame + 6);
  void* volatile * const list_node_ptr =
    (void* volatile *)(closure->tc_frame + 8);
  void* volatile * const id_arr_ptr =
    (void* volatile *)(closure->tc_frame + 10);
  unsigned int used;

  alloc_ptr_array(closure, size);
  alloc_id_array(closure, size);
  used = gc_collection_count & 0x1;
  alloc_graph_node(closure, newid, size, ptr_array_ptr[used]);
  used = gc_collection_count & 0x1;
  alloc_list_node(closure, newid, graph_node_ptr[used], ptr_array[used]);
  used = gc_collection_count & 0x1;
  graph_node_ptr[used] = NULL;
  write_barrier(closure, closure->tc_frame, 4 * sizeof(void**));
  ptr_array_ptr[used] = NULL;
  write_barrier(closure, closure->tc_frame, 6 * sizeof(void**));
  id_array_ptr[used] = NULL;
  write_barrier(closure, closure->tc_frame, 10 * sizeof(void**));

  for(unsigned int i = 1; !try_insert_node(closure); i++) {

    if(15 == i % 16)
      safe_point(closure);

    backoff_delay(i);

  }

  list_node_ptr[used] = NULL;
  write_barrier(closure, closure->tc_frame, 8 * sizeof(void**));
  atomic_increment_uint(&num_nodes);

}


static void* get_node(thread_closure_t* const closure) {

  const unsigned int num = num_nodes.value;
  const unsigned int index = get_random(closure);
  void* volatile * const curr_ptr = (void* volatile *)(closure->tc_frame + 4);
  unsigned int used_ptr;
  bool flipflop;

  flipflop = gc_collection_count & 0x1;
  used_ptr = flipflop ? 0 : 1;
  curr_ptr[used_ptr] = list[used_ptr];

  for(unsigned int i = 0; i < index; i++) {

    void* const curr = curr_ptr[used_ptr];
    void* const data_ptr = (char*)curr + sizeof(gc_header_t);
    void* volatile * const next_ptr =
      (void* volatile *)((unsigned int*)data_ptr + 1);
    void* const next = next_ptr[used_ptr];

    curr_ptr[used_ptr] = next;

    /* XXX remove dead nodes */

    if(NULL == next || index == i)
      break;

    else if(15 == i % 16) {

      write_barrier(closure, closure->tc_frame, 4 * sizeof(void**));
      safe_point(closure);
      flipflop = gc_collection_count & 0x1;
      used_ptr = flipflop ? 0 : 1;

    }

  }

  void* const out = curr_ptr[used_ptr];

  curr_ptr[used_ptr] = NULL;
  write_barrier(closure, closure->tc_frame, 4 * sizeof(void**));

  return out;

}


static void connect(thread_closure_t* const closure) {

  void* const from_node;
  void* const to_node;
  void* volatile * const from_ptr = (void* volatile *)(closure->tc_frame + 6);
  unsigned int used_ptr;
  bool flipflop;

  from_node = get_node(closure);
  flipflop = gc_collection_count & 0x1;
  used_ptr = flipflop ? 0 : 1;
  from_ptr[used_ptr] = from_node;
  write_barrier(closure, closure->tc_frame, 6 * sizeof(void**));
  to_node = get_node(closure);
  flipflop = gc_collection_count & 0x1;
  used_ptr = flipflop ? 0 : 1;

  /* XXX do the ID array */
  if(NULL != curr) {

    void* const data_ptr = (char*)from_ptr + sizeof(gc_header_t);
    void* volatile * const node_ptr =
      (void* volatile *)((unsigned int*)data_ptr + 3);
    void* const node = node_ptr[used_ptr];

    if(NULL != node) {

      void* const node_data_ptr = (char*)node + sizeof(gc_header_t);
      const unsigned int* const size_ptr =
	(const unsigned int*)node_data_ptr + 1;
      void* const * const arr_ptr =
	(void* const *)((unsigned int*)node_data_ptr + 2);
      void* const arr = arr_ptr[used_ptr];
      const unsigned int size = *size_ptr;
      void* volatile * const arr_data_ptr = (char*)node + sizeof(gc_header_t);
      const unsigned int index = get_random(closure) % size;
      void* volatile * const entry = arr_data_ptr + (2 * index);

      entry[used_ptr] = to_ptr;
      /* write barrier on entry */

    }

  }

  from_ptr[used_ptr] = NULL;
  write_barrier(closure, closure->tc_frame, 6 * sizeof(void**));

}


static void disconnect(thread_closure_t* const closure) {

  void* const curr = get_node(closure);
  const bool flipflop = gc_collection_count & 0x1;
  const unsigned int used_ptr = flipflop ? 0 : 1;

  /* XXX do the ID array */
  if(NULL != curr) {

    void* const data_ptr = (char*)curr + sizeof(gc_header_t);
    void* volatile * const node_ptr =
      (void* volatile *)((unsigned int*)data_ptr + 3);
    void* const node = node_ptr[used_ptr];

    if(NULL != node) {

      void* const node_data_ptr = (char*)node + sizeof(gc_header_t);
      const unsigned int* const size_ptr =
	(const unsigned int*)node_data_ptr + 1;
      void* const * const arr_ptr =
	(void* const *)((unsigned int*)node_data_ptr + 2);
      void* const arr = arr_ptr[used_ptr];
      const unsigned int size = *size_ptr;
      void* volatile * const arr_data_ptr = (char*)node + sizeof(gc_header_t);
      const unsigned int index = get_random(closure) % size;
      void* volatile * const entry = arr_data_ptr + (2 * index);

      entry[used_ptr] = NULL;
      /* write barrier on entry */

    }

  }

}


static void change(thread_closure_t* const closure) {

  const unsigned int new = get_random(closure);

  PRINTD("Changing behaviors to %u\n", new);

  behaviors.value = new;

}


static unsigned int select_process(thread_closure_t* const closure) {

  const unsigned int behaviors_value = behaviors.value;
  const unsigned int create_gap = behaviors_value & 0xff;
  const unsigned int connect_gap = (behaviors_value >> 8) & 0xff;
  const unsigned int disconnect_gap = (behaviors_value >> 16) & 0xff;
  const unsigned int change_gap = 4;
  const unsigned int create_limit = create_gap;
  const unsigned int connect_limit = create_limit + connect_gap;
  const unsigned int disconnect_limit = connect_limit + disconnect_gap;
  const unsigned int limit = disconnect_limit + change_gap;
  const unsigned int value = get_random(closure) % limit;
  unsigned int out;

  PRINTD("Create %u(%u), update %u(%u), delete %u(%u), change %u(%u)\n",
	 create_limit, create_gap, connect_limit, connect_gap,
	 disconnect_limit, disconnect_gap, limit, change_gap);

  if(value < check_limit)
    out = 0;

  else if(value < connect_limit)
    out = 1;

  else if(value < disconnect_limit)
    out = 2;

  else
    out = 3;

  return out;

}


static void process(thread_closure_t* const closure) {

  switch(select_process(id)) {

  case 0:

    create(closure);

    break;

  case 1:

    connect(closure);

    break;

  case 2:

    disconnect(closure);

    break;

  default:

    change(closure);

    break;

  }

}
