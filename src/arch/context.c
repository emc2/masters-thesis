/* Copyright (c) 2007 Eric McCorkle.  All rights reserved. */

#include <string.h>
#include "definitions.h"
#include "arch.h"
#include "atomic.h"
#include "cc/context.h"

#if defined(IA_32)
#include "ia32/context.c"
#else
#error "Undefined architecture specification"
#endif


/*!
 * This function pads the given stack pointer by the value given.
 * This uses whatever convention is used by the architecture to ensure
 * safety.  The alignment value must be a power of two.
 *
 * \brief Align a stack pointer.
 * \arg ptr The stack pointer to align.
 * \arg align The amount by which to align it (must be a power of 2).
 * \return The aligned stack pointer.
 */
internal pure volatile void* context_align_stkptr(const volatile void* ptr,
						  unsigned int align) {

  void* out;

  /* stack grows up */
  if(1 == STACK_DIRECTION) {

    const unsigned int ptrval = (unsigned int)ptr;
    const unsigned int aligned = ((ptrval - 1) & ~(align - 1)) + align;

    out = (void*)aligned;

  }

  /* stack grows down */
  else {

    const unsigned int ptrval = (unsigned int)ptr;
    const unsigned int aligned = ptrval & ~(align - 1);

    out = (void*)aligned;

  }

  return out;

}
