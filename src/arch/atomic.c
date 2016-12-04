/* Copyright (c) 2007 Eric McCorkle.  All rights reserved. */

#ifdef IA_32
#include "ia32/atomic.c"
#else
#error "Undefined architecture specification"
#endif
