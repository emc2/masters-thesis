/* Copyright (c) 2007 Eric McCorkle.  All rights reserved. */

#ifndef GC_INTERNAL_H
#define GC_INTERNAL_H

#include "gc.h"

/*!
 * This function initialized the garbage collected memory system to
 * the specified parameters.  The only parameter to this system is the
 * limit on the size of garbage collected memory.  This can be less
 * than the amount given to the slice system, in the event that the
 * desired maximum is ceiling'ed to a larger value.  However, it
 * should never be greater.
 *
 * \brief Initialize the garbage collected memory system.
 * \arg max The maximum amount of memory to allocate 
 */
internal void gc_init(unsigned int max);

/*!
 * This function gets the value of mm_gc_limit.  This is a function
 * to hide access semantics, and also to prevent exporting the value
 * as a symbol (whole program compilers may be able to infer a strict
 * type on this).
 *
 * \brief Get the mm_gc_limit value.
 * \return The value of mm_gc_limit.
 */
internal unsigned int mm_gc_limit(void);

/*!
 * This function gets the value of mm_gc_size.  This is a function
 * to hide access semantics, and also to prevent exporting the value
 * as a symbol.
 *
 * \brief Get the mm_gc_size value.
 * \return The value of mm_gc_size.
 */
internal unsigned int mm_gc_size(void);

/*!
 * This function gets the value of mm_gc_live.  This is a function
 * to hide access semantics, and also to prevent exporting the value
 * as a symbol.
 *
 * \brief Get the mm_gc_live value.
 * \return The value of mm_gc_live.
 */
internal unsigned int mm_gc_live(void);

#endif
