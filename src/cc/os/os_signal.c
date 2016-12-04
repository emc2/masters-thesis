/* Copyright (c) 2007 Eric McCorkle.  All rights reserved. */

#if defined(POSIX)
#include "posix/os_signal.c"
#else
#error "Undefined OS specification"
#endif
