/* Copyright (c) 2007 Eric McCorkle.  All rights reserved. */

#ifndef MALLOC_INTERNAL_H
#define MALLOC_INTERNAL_H

#include "mm_malloc.h"

/*!
 * This function initialized the explicitly managed memory system to
 * the specified parameters.  The only parameter to this system is the
 * limit on the size of explicitly managed memory.  This can be less
 * than the amount given to the slice system, in the event that the
 * desired maximum is ceiling'ed to a larger value.  However, it
 * should never be greater.
 *
 * \brief Initialize the explicitly managed memory system.
 * \arg max The maximum amount of memory to allocate 
 */ 
internal void malloc_init(unsigned int max);

/*!
 * This function gets the value of mm_malloc_limit.  This is a function
 * to hide access semantics, and also to prevent exporting the value
 * as a symbol (whole program compilers may be able to infer a strict
 * type on this).
 *
 * \brief Get the mm_malloc_limit value.
 * \return The value of mm_malloc_limit.
 */
internal unsigned int malloc_limit(void);

/*!
 * This function gets the value of mm_malloc_size.  This is a function
 * to hide access semantics, and also to prevent exporting the value
 * as a symbol.
 *
 * \brief Get the mm_malloc_size value.
 * \return The value of mm_malloc_size.
 */
internal unsigned int malloc_size(void);

/*!
 * This function gets the value of mm_malloc_live.  This is a function
 * to hide access semantics, and also to prevent exporting the value
 * as a symbol.
 *
 * \brief Get the mm_malloc_live value.
 * \return The value of mm_malloc_live.
 */
internal unsigned int malloc_live(void);

#endif
