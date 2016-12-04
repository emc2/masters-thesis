/* Copyright (c) 2007 Eric McCorkle.  All Rights Reserved. */

#ifndef SLICE_H
#define SLICE_H

#include <stdbool.h>

#include "definitions.h"

/*!
 * This is an enum for different types of slices.  Different classes
 * are handled differently by the system.  Types include:
 * - SLICE_TYPE_GC: a garbage-collected slice.
 * - SLICE_TYPE_MALLOC: a malloc-managed slice.
 * - SLICE_TYPE_CUSTOM: a custom-managed slice.
 * - SLICE_TYPE_STATIC: a slice containing data which survives the
 *   duration of the program (this type is usually created upon entry
 *   to the program)
 *
 * \brief Types of slices.
 */
typedef enum {
  SLICE_TYPE_GC,
  SLICE_TYPE_MALLOC,
  SLICE_TYPE_CUSTOM,
  SLICE_TYPE_STATIC
} slice_type_t;

/*!
 * This is an enum for various usage statuses of slices.  This may be
 * one of:
 * - SLICE_USAGE_USED: a slice currently in use
 * - SLICE_USAGE_UNUSED: a slice currently not being used, but holds
 *   meaningful data
 * - SLICE_USAGE_BLANK: a slice which contains no meaningful data, and
 *   may not be paged in at all
 *
 * \brief Usage status of slices. 
 */
typedef enum {
  SLICE_USAGE_USED,
  SLICE_USAGE_UNUSED,
  SLICE_USAGE_BLANK
} slice_usage_t;

/*!
 * This is the memory protection of a slice.  It is defined as an
 * enum, but contrived such that on most systems, it corresponds
 * directly to the protection bit scheme, or a shifted version
 * thereof.  It may be one of:
 * - SLICE_PROT_NONE: inaccessible.
 * - SLICE_PROT_X: executable only.
 * - SLICE_PROT_W: write-only.
 * - SLICE_PROT_WX: write and execute.
 * - SLICE_PROT_R: read-only.
 * - SLICE_PROT_RX: read and execute.
 * - SLICE_PROT_RW: read and write.
 * - SLICE_PROT_RWX: read, write, and execute.
 *
 * \brief The memory protection of a slice.
 */
typedef enum {
  SLICE_PROT_NONE,
  SLICE_PROT_R,
  SLICE_PROT_W,
  SLICE_PROT_RW,
  SLICE_PROT_X,
  SLICE_PROT_RX,
  SLICE_PROT_WX,
  SLICE_PROT_RWX
} slice_prot_t;

/*!
 * This is the maximum allowable slice size.  This varies depending on
 * the architecture.
 *
 * \brief Maximum slice size.
 */
extern const unsigned int slice_max_size;

/*!
 * This is the minimum allowable slice size.  This is usually 4
 * megabytes.
 *
 * \brief Minimum slice size.
 */
extern const unsigned int slice_min_size;

/*!
 * This structure describes a slice.  A slice is a large block of
 * memory allocated from an operating system facility such as mmap.
 * These are the unit of memory used by the top-level memory manager.
 *
 * Under normal circumstances, the memory manager allocates constant
 * (usuall slice_min_size)-sized blocks of memory.  For very large
 * requests, larger blocks may be acquired.
 *
 * \brief A descriptor for a slice.
 */
typedef struct slice_t {

  /*!
   * This is the size of the slice.  It must be between slice_size_min
   * and slice_size_max, page-aligned, and more often than not, a
   * power of two.
   *
   * \brief Size of the slice
   */
  unsigned int s_size;

  /*!
   * This is a pointer to the slice.  Note: the descriptor does not
   * reside in the slice itself, but in a separate table.
   *
   * \brief Pointer to the slice.
   */
  void* s_ptr;

  /*!
   * This is the type of the slice.  It may be one of:
   * - SLICE_TYPE_GC: a garbage-collected slice.
   * - SLICE_TYPE_MALLOC: a malloc-managed slice.
   * - SLICE_TYPE_CUSTOM: a custom-managed slice.
   * - SLICE_TYPE_STATIC: a slice containing data which survives the
   *   duration of the program (this type is usually created upon entry
   *   to the program)
   *
   * \brief Type of the slice.
   */
  slice_type_t s_type;

  /*!
   * This is the current usage state of the slice.  It may be one of:
   * - SLICE_USAGE_USED: a slice currently in use
   * - SLICE_USAGE_UNUSED: a slice currently not being used, but holds
   *   meaningful data
   * - SLICE_USAGE_BLANK: a slice which contains no meaningful data, and
   *   may not be paged in at all
   *
   * \brief Usage state.
   */
  slice_usage_t s_usage;

  /*!
   * This is the memory protections enforced on the slice.  It may be
   * one of:
   * - SLICE_PROT_NONE: inaccessible.
   * - SLICE_PROT_X: executable only.
   * - SLICE_PROT_W: write-only.
   * - SLICE_PROT_WX: write and execute.
   * - SLICE_PROT_R: read-only.
   * - SLICE_PROT_RX: read and execute.
   * - SLICE_PROT_RW: read and write.
   * - SLICE_PROT_RWX: read, write, and execute.
   *
   * \brief Memory protection.
   */
  slice_prot_t s_prot;

  /*!
   * This pointer exists to allow slice descriptors to be arranged
   * into lists.  Usage of this is left up to the individual
   * allocator.
   *
   * \brief A pointer for building lists.
   */
  struct slice_t* s_next;

} slice_t;


typedef struct {

  slice_t* o_value;
  bool o_valid;

} slice_opt_t;


/*!
 * This function initializes the slice allocator system.  It will set
 * the maximum size to the value passed in.  Note that some systems
 * may not be able to allocate this much memory, depending on
 * specifics of the system.  The second parameter sets the default
 * slice size.  The maximum size can be any size, but should be
 * aligned to the default slice size.
 *
 * \brief Initialize the slice allocator.
 * \arg max The maximum size of memory allocated by this system.
 * \arg defaultsize The default slice size.
 * \arg malloc_max The maximum size of malloc memory.
 * \arg gc_max The maximum size of GC memory.
 */
internal void slice_init(unsigned int max, unsigned int defaultsize,
			 unsigned int malloc_max, unsigned int gc_max);

/*!
 * This function attempts to allocate a slice from the operating
 * system.  This may fail for a number of reasons, including lack of
 * available address ranges, lack of memory, or various other causes.
 *
 * \brief Allocate a slice of a given size and class.
 * \arg type The type of the slice to allocate.
 * \arg prot The memory protections to request.
 * \arg size The size of the slice to allocate.
 * \return A slice descriptor.
 */
internal slice_t* restrict slice_alloc(slice_type_t type, slice_prot_t prot,
				       unsigned int size);


/*!
 * This function frees the memory used by a slice and releases its
 * descriptor for use by others.
 *
 * \brief Allocate a slice of a given size and class.
 * \arg type The slice to free.
 */
internal void slice_free(slice_t* restrict slice);


/*!
 * This function sets the usage of a slice.  On platforms where it is
 * supported, the system will also inform the kernel of the change in
 * memory policy.
 *
 * \brief Change the usage of a slice
 * \arg slice The slice to change.
 * \arg usage The new usage policy.
 */
internal void slice_set_usage(slice_t* restrict slice, slice_usage_t usage);

/*!
 * This function sets the memory protections on a slice.
 *
 * Note: some systems may not support memory protection setting (such
 * as standalone systems), or may not support all modes.
 *
 * \brief Change the memory protections on a slice.
 * \arg slice The slice to set.
 * \arg prot The protections to use.
 */
internal void slice_set_prot(slice_t* restrict slice, slice_prot_t prot);

/*!
 * This function gets the value of mm_total_limit.  This is a function
 * to hide access semantics, and also to prevent exporting the value
 * as a symbol (whole program compilers may be able to infer a strict
 * type on this).
 *
 * \brief Get the mm_total_limit value.
 * \return The value of mm_total_limit.
 */
internal unsigned int mm_total_limit(void);

/*!
 * This function gets the value of mm_total_size.  This is a function
 * to hide access semantics, and also to prevent exporting the value
 * as a symbol.
 *
 * \brief Get the mm_total_size value.
 * \return The value of mm_total_size.
 */
internal unsigned int mm_total_size(void);

/*!
 * This function gets the value of mm_slice_size.  This is a function
 * to hide access semantics, and also to prevent exporting the value
 * as a symbol (whole program compilers may be able to infer a strict
 * type on this).
 *
 * \brief Get the mm_slice_size value.
 * \return The value of mm_slice_size.
 */
internal unsigned int mm_slice_size(void);

#endif
