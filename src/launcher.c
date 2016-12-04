/* Copyright (c) 2007, 2008 Eric McCorkle.  All rights reserved. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include "definitions.h"
#include "program.h"
#include "mm.h"
#include "cc.h"
#include "arch.h"

static const char* const usage =
  "Runtime system argument usage:\n"
  " <program> [ @runtime <variable> <value> ]* <program args>\n"
  "\n"
  "Variable name\t\tEnvironment variable\tDescription\n"
  "\n"
  "concurrency_system\tCC_SYSTEM\t\tName of the concurrency system to use\n"
  "memory_manager\t\tMEMORY_MANAGER\t\tName of the memory manager to use\n"
  "cc_num_executors\tCC_NUM_EXECUTORS\tNumber of executors to use\n"
  "cc_executor_stack_size\tCC_EXECUTOR_STACK_SIZE\tSize of executor "
  "call stacks\n"
  "cc_max_threads\t\tCC_MAX_THREADS\t\tMaximum number of threads\n"
  "mm_total_limit\t\tMM_TOTAL_LIMIT\t\tMaximum total dynamic memory\n"
  "mm_gc_limit\t\tMM_GC_LIMIT\t\tMaximum garbage collected memory\n"
  "mm_malloc_limit\t\tMM_MALLOC_LIMIT\t\tMaximum unmanaged memory\n"
  "mm_slice_size\t\tMM_SLICE_SIZE\t\tSize of blocks allocated from OS\n"
  "mm_num_generations\t\tMM_NUM_GENERATIONS\t\tNumber of generations\n"
  "\n";

static mm_stat_t mm_stats = {
  .mm_total_limit = 0,
  .mm_gc_limit = 0,
  .mm_malloc_limit = 0,
  .mm_total_size = 0,
  .mm_gc_size = 0,
  .mm_malloc_size = 0,
  .mm_total_live = 0,
  .mm_gc_live = 0,
  .mm_malloc_live = 0,
  .mm_slice_size = 0
};

static cc_stat_t cc_stats = {
  .cc_num_executors = 0,
  .cc_executor_stack_size = 0,
  .cc_max_threads = 0,
  .cc_num_threads = 0,
};

static char* concurrency_system;
static char* memory_manager;


static inline void print_cc_stats(void) {

  PRINTD("cc_stats = {\n"
	 "  .cc_num_executors = %u\n"
	 "  .cc_executor_stack_size = 0x%x\n"
	 "  .cc_max_threads = %u\n"
	 "  .cc_num_threads = %u\n"
	 "}\nconcurrency_system = \"%s\"\n",
	 cc_stats.cc_num_executors,
	 cc_stats.cc_executor_stack_size,
	 cc_stats.cc_max_threads,
	 cc_stats.cc_num_threads,
	 concurrency_system == NULL ? "default" : concurrency_system);

}


static inline void print_mm_stats(void) {

  PRINTD("mm_stats = {\n"
	 "  .mm_total_limit = 0x%x\n"
	 "  .mm_malloc_limit = 0x%x\n"
	 "  .mm_gc_limit = 0x%x\n"
	 "  .mm_total_size = 0x%x\n"
	 "  .mm_malloc_size = 0x%x\n"
	 "  .mm_gc_size = 0x%x\n"
	 "  .mm_total_live = 0x%x\n"
	 "  .mm_malloc_live = 0x%x\n"
	 "  .mm_gc_live = 0x%x\n"
	 "  .mm_slice_size = 0x%x\n"
	 "  .mm_num_generations = 0x%x\n"
	 "}\nmemory_manager = \"%s\"\n",
	 mm_stats.mm_total_limit,
	 mm_stats.mm_malloc_limit,
	 mm_stats.mm_gc_limit,
	 mm_stats.mm_total_size,
	 mm_stats.mm_malloc_size,
	 mm_stats.mm_gc_size,
	 mm_stats.mm_total_live,
	 mm_stats.mm_malloc_live,
	 mm_stats.mm_gc_live,
	 mm_stats.mm_slice_size,
	 mm_stats.mm_num_generations,
	 memory_manager == NULL ? "default" : memory_manager);

}


static int set_runtime_stats(const int argc, const char* const * const argv,
			     const char** const newargv) {

  unsigned int value;
  char* str;
  int newargc;

  cc_stats.cc_num_executors = default_cc_num_executors;
  cc_stats.cc_executor_stack_size = default_cc_executor_stack_size;
  cc_stats.cc_max_threads = default_cc_max_threads;
  mm_stats.mm_total_limit = default_mm_total_limit;
  mm_stats.mm_malloc_limit = default_mm_malloc_limit;
  mm_stats.mm_gc_limit = default_mm_gc_limit;
  mm_stats.mm_slice_size = default_mm_slice_size;

  /* set from system properties */
  if(NULL != (str = getenv("MM_TOTAL_LIMIT")) && strcmp(str, "")) {

    value = strtoul(str, NULL, 10);

    if(EINVAL != errno)
      if(0 != value)
	mm_stats.mm_total_limit = value;

    else {

      fputs("MM_TOTAL_LIMIT environment variable must be an integer.\n\n",
	    stderr);
      fputs(usage, stderr);
      exit(EXIT_FAILURE);

    }

  }

  if(NULL != (str = getenv("MM_GC_LIMIT")) && strcmp(str, "")) {

    value = strtoul(str, NULL, 10);

    if(EINVAL != errno)
      if(0 != value)
	mm_stats.mm_gc_limit = value;

    else {

      fputs("MM_GC_LIMIT environment variable must be an integer.\n\n",
	    stderr);
      fputs(usage, stderr);
      exit(EXIT_FAILURE);

    }

  }

  if(NULL != (str = getenv("MM_MALLOC_LIMIT")) && strcmp(str, "")) {

    value = strtoul(str, NULL, 10);

    if(EINVAL != errno)
      if(0 != value)
	mm_stats.mm_malloc_limit = value;

    else {

      fputs("MM_MALLOC_LIMIT environment variable must be an integer.\n\n",
	    stderr);
      fputs(usage, stderr);
      exit(EXIT_FAILURE);

    }

  }

  if(NULL != (str = getenv("MM_SLICE_SIZE")) && strcmp(str, "")) {

    value = strtoul(str, NULL, 10);

    if(EINVAL != errno)
      if(0 != value)
	mm_stats.mm_slice_size = value;

    else {

      fputs("MM_SLICE_SIZE environment variable must be an integer.\n\n",
	    stderr);
      fputs(usage, stderr);
      exit(EXIT_FAILURE);

    }

  }

  if(NULL != (str = getenv("MM_NUM_GENERATIONS")) && strcmp(str, "")) {

    value = strtoul(str, NULL, 10);

    if(EINVAL != errno)
      if(0 != value)
	mm_stats.mm_num_generations = value;

    else {

      fputs("MM_NUM_GENERATIONS environment variable must be an integer.\n\n",
	    stderr);
      fputs(usage, stderr);
      exit(EXIT_FAILURE);

    }

  }

  if(NULL != (str = getenv("CC_NUM_EXECUTORS")) && strcmp(str, "")) {

    value = strtoul(str, NULL, 10);

    if(EINVAL != errno)
      if(0 != value)
	cc_stats.cc_num_executors = value;

    else {

      fputs("CC_NUM_EXECUTORS environment variable must be an integer.\n\n",
	    stderr);
      fputs(usage, stderr);
      exit(EXIT_FAILURE);

    }

  }

  if(NULL != (str = getenv("CC_EXECUTOR_STACK_SIZE")) && strcmp(str, "")) {

    value = strtoul(str, NULL, 10);

    if(EINVAL != errno)
      if(0 != value)
	cc_stats.cc_executor_stack_size = value;

    else {

      fputs("CC_EXECUTOR_STACK_SIZE environment variable "
	    "must be an integer.\n\n", stderr);
      fputs(usage, stderr);
      exit(EXIT_FAILURE);

    }

  }

  if(NULL != (str = getenv("CC_MAX_THREADS")) && strcmp(str, "")) {

    value = strtoul(str, NULL, 10);

    if(EINVAL != errno)
      if(0 != value)
	cc_stats.cc_max_threads = value;

    else {

      fputs("CC_MAX_THREADS environment variable must be an integer.\n\n",
	    stderr);
      fputs(usage, stderr);
      exit(EXIT_FAILURE);

    }

  }

  if(NULL != (str = getenv("CC_SYSTEM")) && strcmp(str, ""))
    concurrency_system = str;

  if(NULL != (str = getenv("MEMORY_MANAGER")) && strcmp(str, ""))
    memory_manager = str;

  newargc = 1;
  newargv[0] = argv[0];

  for(int i = 1; i < argc; i++)
    if(strcmp(argv[i], "@runtime"))
      newargv[newargc++] = argv[i];

    else if(argc > ++i) {

      if(!strcmp(argv[i], "cc_num_executors") && argc > ++i) {

	value = strtoul(argv[i], NULL, 10);

	if(EINVAL != errno)
	  if(0 != value)
	    cc_stats.cc_num_executors = value;

	  else {

	    fputs("cc_num_executors argument must be an integer.\n\n", stderr);
	    fputs(usage, stderr);
	    exit(EXIT_FAILURE);

	  }

      }

      else if(!strcmp(argv[i], "cc_executor_stack_size") && argc > ++i) {

	value = strtoul(argv[i], NULL, 10);

	if(EINVAL != errno)
	  if(0 != value)
	    cc_stats.cc_executor_stack_size = value;

	  else {

	    fputs("cc_executor_stack_size argument must be an integer.\n\n",
		  stderr);
	    fputs(usage, stderr);
	    exit(EXIT_FAILURE);

	  }

      }

      else if(!strcmp(argv[i], "cc_max_threads") && argc > ++i) {

	value = strtoul(argv[i], NULL, 10);

	if(EINVAL != errno)
	  if(0 != value)
	    cc_stats.cc_max_threads = value;

	  else {

	    fputs("cc_max_threads argument must be an integer.\n\n", stderr);
	    fputs(usage, stderr);
	    exit(EXIT_FAILURE);

	  }

      }

      else if(!strcmp(argv[i], "mm_total_limit") && argc > ++i) {

	value = strtoul(argv[i], NULL, 10);

	if(EINVAL != errno)
	  if(0 != value)
	    mm_stats.mm_total_limit = value;

	  else {

	    fputs("mm_total_limit argument must be an integer.\n\n", stderr);
	    fputs(usage, stderr);
	    exit(EXIT_FAILURE);

	  }

      }

      else if(!strcmp(argv[i], "mm_malloc_limit") && argc > ++i) {

	value = strtoul(argv[i], NULL, 10);

	if(EINVAL != errno)
	  if(0 != value)
	    mm_stats.mm_malloc_limit = value;

	  else {

	    fputs("mm_malloc_limit argument must be an integer.\n\n", stderr);
	    fputs(usage, stderr);
	    exit(EXIT_FAILURE);

	  }

      }

      else if(!strcmp(argv[i], "mm_gc_limit") && argc > ++i) {

	value = strtoul(argv[i], NULL, 10);

	if(EINVAL != errno)
	  if(0 != value)
	    mm_stats.mm_gc_limit = value;

	  else {

	    fputs("mm_gc_limit argument must be an integer.\n\n", stderr);
	    fputs(usage, stderr);
	    exit(EXIT_FAILURE);

	  }

      }

      else if(!strcmp(argv[i], "mm_slice_size") && argc > ++i) {

	value = strtoul(argv[i], NULL, 10);

	if(EINVAL != errno)
	  if(0 != value)
	    mm_stats.mm_slice_size = value;

	  else {

	    fputs("mm_slice_size argument must be an integer.\n\n", stderr);
	    fputs(usage, stderr);
	    exit(EXIT_FAILURE);

	  }

      }

      else if(!strcmp(argv[i], "mm_num_generations") && argc > ++i) {

	value = strtoul(argv[i], NULL, 10);

	if(EINVAL != errno)
	  if(0 != value)
	    mm_stats.mm_num_generations = value;

	  else {

	    fputs("mm_num_generations argument "
		  "must be an integer.\n\n", stderr);
	    fputs(usage, stderr);
	    exit(EXIT_FAILURE);

	  }

      }

      else {

	fputs("Invalid argument.\n\n", stderr);
	fputs(usage, stderr);
	exit(EXIT_FAILURE);

      }

    }

    else {

      fputs("Bad argument format.\n\n", stderr);
      fputs(usage, stderr);
      exit(EXIT_FAILURE);

    }

  return newargc;

}

int main(const int argc, const char* const * const argv,
	 const char* const * const envp) {

  const char* msg;
  const char* newargv[argc];
  const unsigned int newargc = set_runtime_stats(argc, argv, newargv);
  const unsigned int cc_size = cc_request(&cc_stats);
  const unsigned int mm_size = mm_request(&mm_stats, &cc_stats);

  PRINTD("Reserving 0x%x bytes for memory system.\n",
	 mm_size);
  PRINTD("Reserving 0x%x bytes for concurrency system.\n",
	 cc_size);
  PRINTD("Runtime total static size is 0x%x bytes.\n",
	 mm_size + cc_size);

  void* const restrict mem = mm_start(&mm_stats, &cc_stats, cc_size + mm_size);

  print_mm_stats();
  print_cc_stats();
  srandomdev();
  PRINTD("Starting concurrency system\n");
  cc_start(&cc_stats, prog_main, newargc, newargv, envp, mem);

  return -1;

}
