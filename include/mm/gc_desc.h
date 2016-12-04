/* Copyright (c) 2007, 2008 Eric McCorkle.  All rights reserved. */

#ifndef GC_DESC_H
#define GC_DESC_H

#include "definitions.h"
#include "atomic.h"


/*!
 * This is the version number of the heap format represented by this
 * file.  This is used by the garbage collector to determine if a
 * given program's format is correct for this library.
 *
 * \brief the version of the heap structures in use.
 */
#define GC_HEAP_VERSION 0

#define GCH_COPIED 0x1

#define GC_TYPEDESC_CLASS 0x3
#define GC_TYPEDESC_CONST 0x4

#define GC_WRITE_LOG_LENGTH 64


/*!
 * This enumeration gives the class of a type descriptor.  This is
 * used to distinguish normal object, array, and weak object
 * descriptors.
 *
 * \brief A GC type descriptor class.
 */
typedef enum {
  GC_TYPEDESC_NORMAL = 0,
  GC_TYPEDESC_ARRAY = 1,
} gc_typedesc_class_t;


/*!
 * This is the header for a garbage collected object.  Garbage
 * collected objects must be preceeded by a cache line, or at least 4
 * words which acts as both descriptor and a scratchpad for the
 * garbage collector itself.  The format is as follows:
 *
 * < forwarding pointer, list pointer, type info, misc info >
 *
 * The forwarding pointer is the only multi-writer field.  The list
 * pointer is used by collectors to build their local queues, and is
 * purely thread-local.  The type information is a pointer to a
 * gc_typedesc_t describing the object.
 *
 * The lowest bit of the forwarding pointer encodes whether or not an
 * object has been completely copied to the destination heap.  The
 * second pass of the collector needs to know this, so that it will
 * only preserve the weak pointers for objects that have already been
 * copied, as opposed to copying the whole object needlessly (objects
 * which are introduced in the second pass will still need to be
 * copied, and objects which are modified will be updated by write log
 * execution).  If the object has not been copied, the bit will be
 * XOR'ed with its "expected" value (1 for regular pointers and the
 * all-0 placeholder, 0 for the all-1 placeholder).  The value can be
 * updated without a compare-and-set when the object is finished being
 * copied, because the forwarding pointer does not change once it is
 * set.
 *
 * The miscellaneous information is formatted as follows:
 *
 * < current gen, next gen, count, flags >
 *
 * Each field occupies one byte.  The current generation is stored in
 * the first byte.  The generation in which the object will be
 * allocated on the next collection is stored in the following byte.
 * The number of times the object has survived a collection in the
 * current generation is stored in the third byte.  All other bytes in
 * the word are empty.
 *
 * Under all circumstances, this header is at least the size of a
 * cache line.
 *
 * \brief The header for a garbage collected object.
 */
#if (CACHE_LINE_SIZE / WORD_SIZE <= 4)
typedef void* gc_header_t[4];
#else
typedef void* gc_header_t[CACHE_LINE_SIZE / WORD_SIZE];
#endif


/*!
 * This is a type descriptor for a garbage-collected object.  The
 * compiler generates a static array of these descriptors, which are
 * used by the collector to traverse the memory graph.
 *
 * Pointers to an object always point to its header
 *
 * For GC_TYPEDESC_NORMAL objects, a straightforward interpretation is
 * used.  The layout is as follows:
 *
 * < header, non-pointers, pointers, weak pointers, finalizer? >
 *
 * For GC_TYPEDESC_ARRAY objects, the descriptor describes the format
 * of array elements.  Arrays are preeceeded by a bitmap used for
 * claiming individual elements in a garbage collection pass. The
 * actual pointer is to the normal object header, so the bitmap is
 * effectively a negative-length array.  The format is as follows:
 *
 * < padding, bitmap, length, header,
 * < non-pointers, pointers, weak pointers >*, finalizer? >
 *
 * If an object is marked with a finalizer, then it is followed by a
 * cache line of the format:
 *
 * < function pointer, list pointer, padding >
 *
 * The function pointer is the address of the finalizer function.  The
 * list pointer is used to chain finalizable objects together to
 * create a component of the root set, and to create the "condemned"
 * object list.
 *
 * The memory format for the descriptor itself is as follows:
 *
 * < class/flags, non-pointer bytes, # of pointers, # of weak pointers>
 *
 * \brief A type descriptor for the garbage collector.
 */
typedef unsigned int gc_typedesc_t[4];


/*!
 * This is a double-pointer.  It is two pointer values, one of which
 * is ever actually used.
 *
 * \brief A double-pointer, used for global pointers.
 */
typedef void* gc_double_ptr_t[2];


/*!
 * This function initializes normal object header.  It is used to
 * create a new object in the destination space.  Only type
 * information, the forwarding pointer and extra information is
 * necessary, and the list pointer to NULL.
 *
 * \brief Initialize an array object header.
 * \arg fwd_ptr The forwarding pointer field.
 * \arg type The type information field.
 * \arg flags The flags field.
 * \arg gen The current generation field.
 * \arg next_gen The next generation field.
 * \arg count The count field.
 * \arg header The header to initialize.
 */
internal void gc_header_init_normal(void* fwd_ptr, const unsigned int* type,
				    unsigned char flags, unsigned char gen,
				    unsigned char next_gen, unsigned char count,
				    volatile void* restrict header);


/*!
 * This function initializes an array object header.  It is used to
 * create a new object in the destination space.  Only type
 * information, the forwarding pointer and extra information is
 * necessary, and the list pointer to NULL.
 *
 * \brief Initialize an array object header.
 * \arg fwd_ptr The forwarding pointer field.
 * \arg type The type information field.
 * \arg flags The flags field.
 * \arg gen The current generation field.
 * \arg next_gen The next generation field.
 * \arg count The count field.
 * \arg len The length of the array.
 * \arg copied Whether or not the object has been copied in full.
 * \arg header The header to initialize.
 */
internal void gc_header_init_array(void* fwd_ptr, const unsigned int* type,
				   unsigned char flags, unsigned char gen,
				   unsigned char next_gen, unsigned char count,
				   unsigned int len,
				   volatile void* restrict header);


/*!
 * This function returns the forwarding pointer field of a garbage
 * collection header.  This is the pointer to the new copy of the
 * object.  When this particular object is unclaimed, this value is ~0
 * (two's compliment -1) or 0, depending on GC_BEGIN_FLIP_FLOP.  When
 * it is claimed, the value is either a valid pointer, or the opposite
 * of the unclaimed value.
 *
 * \brief Get the forwarding pointer from an object header.
 * \arg header The object header.
 * \return The forwarding pointer's value.
 */
internal void* gc_header_fwd_ptr(volatile void* restrict header);


/*!
 * This function sets the forwarding pointer field of a garbage
 * collection header.
 *
 * \brief Set the forwarding pointer in an object header.
 * \arg ptr The pointer value to which to set the field.
 * \arg header The header to modify.
 */
internal void gc_header_fwd_ptr_set(void* ptr, volatile void* restrict header);


/*!
 * This function performs a compare-and-set operation on the forwaring
 * pointer field.
 *
 * \brief Perform a compare-and-set on the forwarding pointer.
 * \arg expect The expected value.
 * \arg expect_copied The expected value of the copied field.
 * \arg value The new value.
 * \arg expect_value The new value of the copied field.
 * \arg header The header.
 * \return Whether the operation succeeded.
 */
internal bool gc_header_compare_and_set_fwd_ptr(void* expect, void* value,
						volatile void* restrict header);


/*!
 * This function returns the list pointer field of a garbage
 * collection header.  This is the pointer space used by individual
 * executors to build their queues.  This is not a volatile field.
 *
 * \brief Get the list pointer from an object header.
 * \arg header The object header.
 * \return The list pointer's value.
 */
internal pure void* gc_header_list_ptr(volatile void* restrict header);


/*!
 * This function sets the list pointer field of a garbage collection
 * header.
 *
 * \brief Set the list pointer in an object header.
 * \arg ptr The pointer value to which to set the field.
 * \arg header The header to modify.
 */
internal void gc_header_list_ptr_set(void* ptr, volatile void* restrict header);


/*!
 * This function returns the pointer to type information for this
 * object.  The type information describes the format of data after
 * the header.
 *
 * \brief Get the type information from an object header.
 * \arg header The object header.
 * \return The type index.
 */
internal pure unsigned int* gc_header_type(volatile void* restrict header);


/*!
 * This function returns the flags from a garbage collection header.
 * Valid flags are:
 *
 * This field is for all intents and purposes constant.  Mutators
 * (guest program threads) may set certain flags, but the collector is
 * not required to acknowledge those flags in any particular fashion.
 *
 * \brief Get the flags from an object header.
 * \arg header The object header.
 * \return The flags.
 */
internal unsigned char gc_header_flags(volatile void* restrict header);


/*!
 * This function returns the array length from a garbage collection
 * header.  This function must be used only on an array header.  If
 * used on any other type of header, the value returned has no
 * meaning.
 *
 * \brief Get the array length from an object header.
 * \arg header The object header (must be an array).
 * \return The array length field.
 */
internal pure unsigned int gc_header_array_len(volatile void* restrict header);


/*!
 * This function returns the current generation from a garbage
 * collection header.  This function must be used only on a normal
 * header.  If used on any other type of header, the value returned
 * has no meaning.
 *
 * \brief Get the current generation from an object header.
 * \arg header The object header (must be a normal object).
 * \return The current generation field.
 */
internal pure unsigned int gc_header_curr_gen(volatile void* restrict header);


/*!
 * This function returns the next generation from a garbage
 * collection header.  This function must be used only on a normal
 * header.  If used on any other type of header, the value returned
 * has no meaning.
 *
 * \brief Get the next generation from an object header.
 * \arg header The object header (must be a normal object).
 * \return The next generation field.
 */
internal unsigned int gc_header_next_gen(volatile void* restrict header);


/*!
 * This function returns the collection count from a garbage
 * collection header.  This states the number of times an object has
 * survived collection in the current generation.  This function must
 * be used only on a normal header.  If used on any other type of
 * header, the value returned has no meaning.
 *
 * \brief Get the collection count from an object header.
 * \arg header The object header (must be a normal object).
 * \return The collection count field.
 */
internal pure unsigned int gc_header_count(volatile void* restrict header);


/*!
 * This function returns the class from a type descriptor.  The
 * following values are possible:
 *
 * - GC_TYPEDESC_NORMAL: Represents a normal object.
 * - GC_TYPEDESC_ARRAY: Represents an array.
 *
 * This field, like all fields in this structure, is constant.
 *
 * \brief Get the class from a type descriptor.
 * \arg desc The type descriptor.
 * \return The object class.
 */
internal pure gc_typedesc_class_t gc_typedesc_class(const unsigned int*
						    restrict desc);


/*!
 * This function returns the flags from a type descriptor.  Valid
 * flags include:
 *
 * - GC_TYPEFLAG_CONST: The object contains no mutable fields.
 *
 * This field, like all fields in this structure, is constant.
 *
 * \brief Get the flags from a type descriptor.
 * \arg desc The type descriptor.
 * \return The type flags.
 */
internal pure unsigned int gc_typedesc_flags(const unsigned int* restrict desc);


/*!
 * This function returns the number of non-pointer bytes present in
 * this type of object.  This number of bytes appears at the beginning
 * of the object in memory.
 *
 * This field, like all fields in this structure, is constant.
 *
 * \brief Get the non-pointer data size from a type descriptor.
 * \arg desc The type descriptor.
 * \return The number of non-pointer bytes.
 */
internal pure unsigned int gc_typedesc_nonptr_size(const unsigned int*
						   restrict desc);


/*!
 * This function returns the number of normal pointers present in the
 * object.  This is the number of pointers, not the size of pointer
 * space.  These pointers follow the non-pointer data.  Null pointers
 * are not traversed.
 *
 * This field, like all fields in this structure, is constant.
 *
 * \brief Get the number of normal pointers from a type descriptor.
 * \arg desc The type descriptor.
 * \return The number of normal pointers.
 */
internal pure unsigned int gc_typedesc_normal_ptrs(const unsigned int*
						   restrict desc);


/*!
 * This function returns the number of weak pointers present in the
 * object.  These follow the normal pointers, and are ignored by some
 * passes of the garbage collector.  Null pointers are never
 * traversed.
 *
 * This field, like all fields in this structure, is constant.
 *
 * \brief Get the number of weak pointers from a type descriptor.
 * \arg desc The type descriptor.
 * \return The number of weak pointers.
 */
internal pure unsigned int gc_typedesc_weak_ptrs(const unsigned int*
						 restrict desc);


/*!
 * This is a write log entry, which is used to implement the
 * write-barrier.  These are accumulated per-executor, then processed
 * upon entry into the collector thread.  The memory format is as
 * follows:
 *
 * < object pointer, offset >
 *
 * If offset is within the non object-pointer values, then the old
 * value field must be the pointer equivalent of -1, which will
 * inhibit the collector from attempting to traverse the new value.
 * Old values of NULL denote non-traversable pointers in any case.
 *
 * For multi-word updates to non object-pointer data, multiple entries
 * are generated.
 *
 * \brief A write log entry.
 */
typedef void* gc_log_entry_t[2];


/*!
 * This function returns the object pointer from a log entry.  This is
 * actually a pointer to the object's header.
 *
 * \brief Get the object pointer from a log entry.
 * \arg entry The log entry.
 * \return A pointer to the object's header.
 */
internal pure void* gc_log_entry_objptr(const void* restrict entry);


/*!
 * This function returns the offset into the object at which the
 * modification occurred.  This does not include the size of the
 * object header.
 *
 * \brief Get the offset of the modified location from a log entry.
 * \arg entry The log entry.
 * \return The offset of the location which was modified.
 */
internal pure unsigned int gc_log_entry_offset(const void* restrict entry);

#endif
