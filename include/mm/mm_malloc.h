/* Copyright (c) 2007 Eric McCorkle.  All rights reserved. */

#ifndef MM_MALLOC_H
#define MM_MALLOC_H


/*!
 * This function calculates the required size of statically-allocated
 * structures used by the memory allocator which must be created at
 * runtime.  Some structures' sizes cannot be determined at compile
 * time, and a slice must be allocated for them at runtime.  This
 * function facilitates this.
 *
 * This function must return a multiple of CACHE_LINE_SIZE.
 *
 * \brief Return the size of memory the explicit allocator needs for
 * its static structures.
 * \arg exec The number of executors.
 * \return The size of all static structures belonging to malloc
 */
internal unsigned int mm_malloc_request(unsigned int exec);


/*!
 * This function initializes the explicit memory allocator.  This must
 * be called prior to using any malloc-family functions.  The pointer
 * given to the function is a pointer to the number of bytes returned
 * by mm_malloc_size, with the same argument.
 *
 * \brief Initialize the explicit memory allocator.
 * \arg exec The number of executors.
 * \arg mem The statically allocated memory available to the
 * subsystem.
 * \return The next section of memory to use.
 */
internal void* mm_malloc_init(unsigned int exec, void* mem);


/*!
 * This function shuts down the explicity memory allocator.  This will
 * destroy all memory allocated with malloc.  It must be called when
 * no such memory can be accessed.
 *
 * \brief Shutdown the explicit memory allocator.
 */
internal void mm_malloc_shutdown(void);


/*!
 * This function is the backend of malloc.  The function also behaves
 * exactly like the stdlib malloc, but takes the ID of the current
 * executor.  This is to allow the rest of the runtime to allocate
 * memory without calling cc_executor_id.  All runtime functions
 * should use this function, not malloc.
 *
 * \brief Allocate memory.
 * \arg size The size of the block to allocate.
 * \arg exec The ID of the current executor.
 * \return A pointer to a block of at least size bytes, or NULL.
 */
extern void* malloc_lf(unsigned int size, unsigned int exec);

/*!
 * This function is the backend of free.  The function behaves exactly
 * as the stdlib free function.  This particular implementation is a
 * lock-free algorithm, and freeing memory may actually shrink the
 * program's heap.  As per stdlib, freeing a NULL pointer has no
 * effect.  This function allows the runtime to avoid calling
 * cc_executor_id.  All runtime functions should use this, not free.
 *
 * \brief Free the memory pointed to by ptr.
 * \arg exec The ID of the current executor.
 * \arg ptr The pointer to the memory to free, or NULL.
 */
extern void free_lf(void* ptr, unsigned int exec);

/*!
 * This function releases unused memory from explicitly managed
 * memory.  It will attempt to confine the total memory size to the
 * given argument, or to an automatically calculated value if given a
 * zero argument.  This is generally less effective than compacting
 * the garbage collected memory.
 *
 * This call will not necessarily wait until compaction is completed
 * to return.  It will start threads to manage the compaction.
 *
 * \brief Release unused memory from the explicitly managed portion of
 * the memory management system.
 * \arg size The size to attempt to reach for the entire memory state,
 * or 0.
 */
extern void malloc_compact(unsigned int size);

#endif
