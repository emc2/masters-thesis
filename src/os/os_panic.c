/* Copyright (c) 2008 Eric McCorkle.  All rights reserved. */

#if defined(POSIX)
#include "posix/os_panic.c"
#else
#error "Undefined OS specification"
#endif
