/* Copyright (c) 2008 Eric McCorkle.  All rights reserved. */

#include <stdlib.h>

#include "definitions.h"
#include "atomic.h"
#include "mm/gc_desc.h"


internal void gc_header_init_normal(void* const fwd_ptr,
				    const unsigned int* const type,
				    const unsigned char flags,
				    const unsigned char gen,
				    const unsigned char next_gen,
				    const unsigned char count,
				    volatile void* const restrict header) {

  void* volatile * const fwd_ptr_ptr = (void* volatile *)header;
  void** const list_ptr_ptr = (void**)header + 1;
  unsigned int* const typeinfo_ptr = (unsigned int*)header + 2;
  unsigned int* const geninfo_ptr = (unsigned int*)header + 3;
  void* const list_ptr = NULL;
  const unsigned int geninfo =
    gen | (next_gen << 8) | (count << 16) | (flags << 24);

  *fwd_ptr_ptr = fwd_ptr;
  *list_ptr_ptr = list_ptr;
  *typeinfo_ptr = (unsigned int)type;
  *geninfo_ptr = geninfo;

}


internal void gc_header_init_array(void* const fwd_ptr,
				   const unsigned int* const type,
				   const unsigned char flags,
				   const unsigned char gen,
				   const unsigned char next_gen,
				   const unsigned char count,
				   const unsigned int len,
				   volatile void* const restrict header) {

  unsigned int* const len_ptr = (unsigned int*)header + 2;
  void* volatile * const fwd_ptr_ptr = (void* volatile *)header;
  void** const list_ptr_ptr = (void**)header + 1;
  unsigned int* const typeinfo_ptr = (unsigned int*)header + 2;
  unsigned int* const geninfo_ptr = (unsigned int*)header + 3;
  void* const list_ptr = NULL;
  const unsigned int geninfo =
    gen | (next_gen << 8) | (count << 16) | (flags << 24);

  *len_ptr = len;
  *fwd_ptr_ptr = fwd_ptr;
  *list_ptr_ptr = list_ptr;
  *typeinfo_ptr = (unsigned int)type;
  *geninfo_ptr = geninfo;

}


internal void* gc_header_fwd_ptr(volatile void* const restrict header) {

  void* volatile * const forward_ptr_ptr = (void* volatile *)header;

  return *forward_ptr_ptr;

}


internal void gc_header_fwd_ptr_set(void* const ptr,
				    volatile void* const restrict header) {

  void** const fwd_ptr_ptr = (void**)header;

  *fwd_ptr_ptr = ptr;

}


internal bool gc_header_compare_and_set_fwd_ptr(void* const expect,
						void* const value,
						volatile void* const
						restrict header) {

  void* volatile * const forward_ptr_ptr = (void* volatile *)header;

  return atomic_compare_and_set_ptr(expect, value, forward_ptr_ptr);

}


internal pure void* gc_header_list_ptr(volatile void* const restrict header) {

  void** const list_ptr_ptr = (void**)header + 1;

  return *list_ptr_ptr;

}


internal void gc_header_list_ptr_set(void* const ptr,
				     volatile void* const restrict header) {

  void** const list_ptr_ptr = (void**)header + 1;

  *list_ptr_ptr = ptr;

}


internal pure unsigned int* gc_header_type(volatile void* const
					   restrict header) {

  unsigned int* const typeinfo_ptr = (unsigned int*)header + 2;

  return (unsigned int*)(*typeinfo_ptr & ~0xf);

}


internal unsigned char gc_header_flags(volatile void* const restrict header) {

  unsigned int* const misc_info_ptr = (unsigned int*)header + 3;

  return ((*misc_info_ptr) >> 24) & 0xff;

}


internal void gc_header_flags_set(volatile void* const restrict header,
				  const unsigned char flags) {

  unsigned* const misc_info_ptr = (unsigned int*)header + 3;
  const unsigned int misc_info = *misc_info_ptr & ~0xff000000;

  *misc_info_ptr = misc_info | (flags << 24);

}


internal pure unsigned int gc_header_curr_gen(volatile void* const
					      restrict header) {

  unsigned int* const geninfo_ptr = (unsigned int*)header + 3;

  return *geninfo_ptr & 0xff;

}


internal pure unsigned int gc_header_next_gen(volatile void* const
					      restrict header) {

  unsigned int* const geninfo_ptr = (unsigned int*)header + 3;

  return (*geninfo_ptr & 0xff) >> 8;

}


internal pure unsigned int gc_header_count(volatile void* const
					   restrict header) {

  unsigned int* const geninfo_ptr = (unsigned int*)header + 3;

  return (*geninfo_ptr & 0xff) >> 16;

}


internal pure unsigned int gc_header_array_len(volatile void* const
					       restrict header) {

  unsigned int* const extra_ptr = (unsigned int*)header - 1;

  return *extra_ptr;

}


internal pure gc_typedesc_class_t gc_typedesc_class(const unsigned int* const
						    restrict desc) {

  return (gc_typedesc_class_t)(desc[0] & GC_TYPEDESC_CLASS);

}


internal pure unsigned int gc_typedesc_flags(const unsigned int* const
					     restrict desc) {

  return desc[0] & ~GC_TYPEDESC_CLASS;

}


internal pure unsigned int gc_typedesc_nonptr_size(const unsigned int* const
						   restrict desc) {

  return desc[1];

}


internal pure unsigned int gc_typedesc_normal_ptrs(const unsigned int* const
						   restrict desc) {

  return desc[2];

}


internal pure unsigned int gc_typedesc_weak_ptrs(const unsigned int* const
						 restrict desc) {

  return desc[3];

}


internal pure void* gc_log_entry_objptr(const void* const restrict entry) {

  void* const * const objptr_ptr = entry;

  return *objptr_ptr;

}


internal pure unsigned int gc_log_entry_offset(const void* const
					       restrict entry) {

  const unsigned int* const offset_ptr = (unsigned int*)entry + 1;

  return *offset_ptr;

}
