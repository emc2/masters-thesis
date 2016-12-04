/* Copyright (c) 2007 Eric McCorkle.  All rights reserved. */
#ifdef POSIX
#include "posix/os_thread.c"
#else
#error "Undefined OS specification"
#endif
