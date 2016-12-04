/* Copyright (c) 2008 Eric McCorkle.  All rights reserved. */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

internal noreturn void panic(const char* restrict str, ...) {

  va_list args;

  va_start(args, str);
  vfprintf(stderr, str, args);
  va_end(args);
  abort();

}
