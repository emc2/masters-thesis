/* Copyright (c) 2008 Eric McCorkle.  All rights reserved */

#ifndef GC_ALLOC_H
#define GC_ALLOC_H

#include "definitions.h"
#include "mm/gc_thread.h"


/*!
 * This function refreshes alloc with a new block of at least min
 * bytes.  The block will be approximately target bytes in size, but
 * no less than min.  This call may begin a collection, and may block
 * the thread to wait on the collection in progress to terminate.  If
 * the allocation simply cannot be performed, EXCEPT_OUT_OF_MEMORY
 * will be signalled with exception().
 *
 * \brief Refresh an allocator's memory.
 * \arg alloc All allocators for the executor.
 * \arg min The minimum number of bytes in the new block.
 * \arg target The target numbe of bytes in the new block.
 * \arg gen The generation store which is being refreshed.
 */
extern bool gc_allocator_refresh(gc_allocator_t* allocators, unsigned int min,
				 unsigned int target, unsigned int gen,
				 unsigned int exec);


/*!
 * This function allocates size bytes from alloc.  This is intended
 * for internal use only.  External resources should update the
 * allocators for an executor themselves.
 *
 * \brief Allocate garbage-collected memory.
 * \arg allocator The allocator to use.
 * \arg size The number of bytes to allocate.
 * \arg gen The generation store from which to allocate.
 * \return The allocated memory, or NULL if the call fails.
 */
internal void* gc_allocator_alloc(gc_allocator_t allocator,
				  unsigned int size,
				  unsigned int gen);


/*!
 * This function preallocates size bytes from alloc, which must be the
 * new-space allocator for a garbage collection thread.  This will use
 * free slices past the limit if it runs out of memory, and will add
 * any slices used to the list of new-space slices.
 *
 * Preallocation reserves all space and updates the allocators, but
 * does not actually advance the allocation pointer.  The pointer
 * returned can be used, but must be postallocated before another
 * block is preallocated if it is to remain persistent.
 *
 * \brief Preallocate for garbage collection.
 * \arg closure The thread performing the allocation.
 * \arg size The number of bytes to allocate.
 * \arg gen The generation store from which to allocate.
 * \return The allocated memory, or NULL if the call fails.
 */
internal void* gc_allocator_gc_prealloc(gc_closure_t* restrict closure,
					unsigned int size, unsigned int gen);


/*!
 * This function postallocates size bytes from alloc.  The allocator
 * must have preallocated size bytes.  Postallocation advances the
 * allocation pointer, finalizing the allocation.
 *
 * \brief Postallocate for garbage collection.
 * \arg closure The thread performing the allocation.
 * \arg size The number of bytes to allocate.
 * \arg gen The generation store from which to allocate.
 */
internal void gc_allocator_gc_postalloc(gc_closure_t* restrict closure,
					unsigned int size, unsigned int gen);


/*!
 * This function releases all slices which were marked as used, moving
 * them to the free status.  The new-space slices are moved to being
 * in the used status.
 *
 * This function is not atomic.  It is called only by the executor
 * which is the last to pass the final barrier, so it may access
 * memory in a non-atomic manner.
 *
 * \brief Release previously used memory.
 * \arg gen The highest generation store which will be released.
 */
internal void gc_allocator_release_slices(unsigned int gen);

#endif
