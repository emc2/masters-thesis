/* Copyright (c) 2007 Eric McCorkle.  All rights reserved. */

#include <dlfcn.h>
#include "definitions.h"
#include "os_dl.h"

/*!
 * This function is a wrapper for dlopen or the like on the host
 * platform.  It opens the dynamic library requested, as if it were
 * loaded in RTLD_LAZY | RTLD_LOCAL modes.  However, it searches for
 * libraries in a slightly different manner.
 *
 * As this is for a runtime system, it is highly susceptible to
 * library intercept attacks.  As such, this function only searches a
 * set number of locations.  It will prepend the directories, and
 * append whatever platform-specific extension is necessary (usually
 * .so, .dll, or .dylib).  However, the usual platform-specific prefix
 * (lib on POSIX) will not be prepended.
 *
 * \brief Load a dynamic library.
 * \arg name The name of the dynamic library to load.
 * \return A dl_result_t containing a handle for the library.
 */
internal dl_result_t os_dlopen(const char* restrict name) {

  INVARIANT(name != NULL);

  dl_result_t out;

  out.dl_addr = dlopen(name, RTLD_LAZY | RTLD_LOCAL);
  out.dl_error = dlerror();

  return out;

}


/*!
 * This function gets a symbol from a dynamic library's handle.  It is
 * analogous to the dlsym function found in POSIX environments.
 *
 * \brief Get a symbol from a dynamic library.
 * \arg dlib A handle, as returned by dlopen.
 * \arg name The name of the symbol to fetch.
 * \return A dl_result_t containing the address of the symbol.
 */
internal dl_result_t os_dlsym(void* dlib, const char* restrict name) {

  INVARIANT(dlib != NULL);
  INVARIANT(name != NULL);

  dl_result_t out;

  out.dl_addr = dlsym(dlib, name);
  out.dl_error = dlerror();

  return out;

}


/*!
 * This function closes a dynamic library handle.  Its behavior is
 * equivalent to the dlclose function in POSIX environments.  If the
 * reference count for the library reaches 0, it is released from the
 * address space, and all references to its contents become invalid.
 *
 * \brief Close a dynamic library.
 * \arg dlib The handle to close.
 * \return The error string, if an error occurred, or NULL if no error
 * occurred.
 */
internal const char* restrict os_dlclose(void* dlib) {

  INVARIANT(dlib != NULL);

  dlclose(dlib);

  return dlerror();

}
