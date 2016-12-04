#include <stdlib.h>
#include "cc.h"
#include "program.h"

const unsigned int default_cc_num_executors = 0;
const unsigned int default_cc_executor_stack_size = 0;
const unsigned int default_cc_max_threads = 0;
const unsigned int default_mm_total_limit = 0;
const unsigned int default_mm_malloc_limit = 0;
const unsigned int default_mm_gc_limit = 0;
const unsigned int default_mm_slice_size = 0x40000;
const unsigned int default_gc_gens = 3;
const unsigned int default_gc_array_gen = 2;
const gc_typedesc_t gc_types[0] = {};
gc_double_ptr_t* const gc_global_ptrs[0] = {};
const unsigned int gc_global_ptr_count = 0;

noreturn void prog_main(thread_t* const restrict thread,
			const unsigned int exec,
			const unsigned int argc,
			const char* const * const argv,
			const char* const * const envp) {

  cc_stop(exec);

}
