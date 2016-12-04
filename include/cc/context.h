/* Copyright (c) 2007 Eric McCorkle.  All rights reserved. */

#ifndef CONTEXT_H
#define CONTEXT_H

#include "definitions.h"
#include "arch.h"


typedef char context_t[1];

/*!
 * This function jumps to the given context.  It does not return, nor
 * does it necessarily "consume" the context.  On a traditional stack,
 * the context will likely be invalid after a short time.  On cactus
 * stacks, it may not be.
 *
 * \brief Load the given context.
 * \arg retaddr The return address of the context.
 * \arg frame The stack pointer or frame address of the context.
 */
internal noreturn void context_load(void (*retaddr)(void),
				    volatile void* frame);


/*!
 * This "function" returns the present value of the stack pointer.
 * Note: this depends heavily on inlining to turn this into a simple
 * value.
 *
 * \brief Get the current stack pointer.
 * \return The current value of the stack pointer.
 */
internal volatile void* context_curr_stkptr(void);


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
						  unsigned int align);

#endif
