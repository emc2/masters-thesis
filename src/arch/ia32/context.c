/* Copyright (c) Eric McCorkle 2007, 2008.  All rights reserved. */

#include <stdlib.h>
#include "definitions.h"
#include "arch.h"
#include "cc/context.h"

internal noreturn void context_load(void (*const retaddr)(void),
				    volatile void* const frame) {

  asm volatile ("mov       %1, %%esp\n\t"
		"addl      $-4, %%esp\n\t"
		"jmp       *%0"
		:
		: "r"(retaddr), "r"(frame));

  /* Control should never get here */
  abort();
  exit(-1);

}


internal volatile void* context_curr_stkptr(void) {

  volatile void* out;

  asm("mov      %%esp, %0"
      : "=r"(out));

  return out;

}
