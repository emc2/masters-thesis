/* Copyright (c) 2008 Eric McCorkle.  All rights reserved. */

#include "definitions.h"
#include "bitops.h"

internal unsigned char bitscan_high(unsigned int word) {

  unsigned int out;

  asm("bsr      %1, %0"
      : "=r"(out)
      : "r"(word));

  return out;

}
