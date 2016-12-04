/* Copyright (c) 2007 Eric McCorkle.  All rights reserved. */

#if defined(POSIX)
#include "posix/os_dl.c"
#else
#error "Undefined OS specification"
#endif
