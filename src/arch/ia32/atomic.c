/* Copyright (c) 2007, 2008 Eric McCorkle.  All rights reserved. */

#include <stdio.h>
#include <stdlib.h>

#include "definitions.h"
#include "atomic.h"


internal bool atomic_compare_and_set_uint(const unsigned int expect,
					  const unsigned int value,
					  volatile void* const restrict ptr) {

  bool out;

  asm("lock\n\t"
      "cmpxchgl  %2, %3\n\t"
      "setz      %0"
      : "=r"(out)
      : "a"(expect), "r"(value), "m"(*((unsigned int*)ptr))
      : "cc");

  return out;

}


internal unsigned int atomic_fetch_inc_uint(volatile void* const restrict ptr) {

  unsigned int out;

  asm("movl      $1, %0\n\t"
      "lock\n\t"
      "xadd      %0, %1"
      : "+r"(out)
      : "m"(*((unsigned int*)ptr)));

  return out + 1;

}


internal unsigned int atomic_fetch_dec_uint(volatile void* const restrict ptr) {

  unsigned int out;

  asm("movl      $-1, %0\n\t"
      "lock\n\t"
      "xadd      %0, %1"
      : "+r"(out)
      : "m"(*((unsigned int*)ptr)));

  return out - 1;

}


internal void atomic_increment_uint(volatile void* const restrict ptr) {

  asm("lock\n\t"
      "incl      %0"
      :
      : "m"(*((unsigned int*)ptr)));

}


internal void atomic_decrement_uint(volatile void* const restrict ptr) {

  asm("lock\n\t"
      "decl      %0"
      :
      : "m"(*((unsigned int*)ptr)));

}


internal bool atomic_compare_and_set_uint64(const uint64_t expect,
					    const uint64_t value,
					    volatile void* const restrict ptr) {

  bool out;

  asm("lock\n\t"
      "cmpxchg8b %4\n\t"
      "setz      %0"
      : "=r"(out)
      : "A"(expect), "b"((uint32_t)value),
        "c"((uint32_t)(value >> 32)), "m"(*((uint64_t*)ptr))
      : "cc");

  return out;

}


internal uint64_t atomic_read_uint64(volatile void* const restrict value) {

  uint64_t out;

  asm("movq      %1, %%mm0\n\t"
      "movd      %%mm0, %%eax\n\t"
      "punpckhdq %%mm0, %%mm0\n\t"
      "movd      %%mm0, %%edx"
      : "=A"(out)
      : "m"(*((uint64_t*)value))
      : "%mm0");

  return out;

}


internal bool atomic_compare_and_set_ptr(void* const expect,
					 void* const value,
					 volatile void* const restrict ptr) {

  bool out;

  asm("lock\n\t"
      "cmpxchgl  %2, %3\n\t"
      "setz      %0"
      : "=r"(out)
      : "a"(expect), "r"(value), "m"(*((void**)ptr))
      : "cc");

  return out;

}


static inline pure unsigned int bsf(const unsigned int value) {

  unsigned int out;

  asm("bsf       %1, %0"
      : "=r"(out)
      : "r"(value));

  return out;

}


internal int atomic_bitmap_alloc(volatile unsigned int* const restrict bitmap,
				 const unsigned int bits, const bool clear) {

  const unsigned int value = *bitmap;
  const unsigned int bit = bsf(value);
  int out;

  if(0 != bit) {

    const unsigned int mask = 1 << bit;
    const unsigned int newvalue = value | mask;
    bool succeed;

    asm("lock\n\t"
	"cmpxchgl  %2, %3\n\t"
	"setz      %0"
	: "=r"(succeed)
	: "a"(value), "r"(newvalue), "m"(*bitmap)
	: "cc");

    out = succeed ? bit : -2;

  }

  else
    out = -1;

  return out;

}


static inline bool try_atomic_bitmap_free(volatile unsigned int*
					  const restrict bitmap,
					  const unsigned int mask) {

  const unsigned int value = *bitmap;
  const unsigned int newvalue = value & ~mask;
  bool out;

  asm("lock\n\t"
      "cmpxchgl  %2, %3\n\t"
      "setz      %0"
      : "=r"(out)
      : "a"(value), "r"(newvalue), "m"(*bitmap)
      : "cc");

  return out;

}


internal void atomic_bitmap_free(volatile unsigned int* const restrict bitmap,
				 const unsigned int bit, const bool clear) {

  const unsigned int byte_index = bit / sizeof(unsigned int);
  const unsigned int bit_index = bit % sizeof(unsigned int);
  const unsigned int mask = 1 << bit;

  for(unsigned int i = 0;
      !try_atomic_bitmap_free(bitmap + byte_index, mask);
      i++)
    backoff_delay(i);


}


internal void load_fence(void) {

  asm volatile ("lfence");

}


internal void store_fence(void) {

  asm volatile ("sfence");

}


internal void mem_fence(void) {

  asm volatile ("mfence");

}


internal void inst_fence(void) {

  mem_fence();

}


internal void backoff_delay(const unsigned int n) {

  if(n < 8)
    asm volatile ("pause");

  else if(n < 64) {

    /* A very crude approximation of a random exponential backoff,
     * designed not to take too much time.
     */
    const unsigned int exp = n / 8;
    const unsigned int spin = 1 << exp + ((n & 0x7) ^ 0x5);

    asm volatile ("pause\n\t"
		  "L_%=:\tsub      $1, %0\n\t"
		  "jnz      L_%=\n\t"
		  "pause"
		  :
		  : "r"(spin));

  }

  else {

    /* An actual random exponential backoff */
    const unsigned int exp = n / 8;
    const unsigned int spin = 1 << (exp < 13 ? exp : 13);
    const unsigned int rand = (random() & ((spin >> 1) - 1));
    const unsigned int rand_spin = spin + rand;

    asm volatile ("pause\n\t"
		  "L_%=:\tsub      $1, %0\n\t"
		  "jnz      L_%=\n\t"
		  "pause"
		  :
		  : "r"(rand_spin));

  }

}
