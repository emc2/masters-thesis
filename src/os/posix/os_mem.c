#include <sys/mman.h>
#include "os_mem.h"

static const int prot_map[] = {
  PROT_NONE,
  PROT_EXEC,
  PROT_WRITE,
  PROT_WRITE | PROT_EXEC,
  PROT_READ,
  PROT_READ | PROT_EXEC,
  PROT_READ | PROT_WRITE,
  PROT_READ | PROT_WRITE | PROT_EXEC
};

/*!
 * This function requests a block of memory from the operating system
 * of the given size and protections.  The size must generally be
 * page-aligned, and the protections may be altered due to OS
 * restrictions.
 *
 * \brief Request a block of memory.
 * \arg size The size of the block.
 * \arg prot The protections of the block.
 * \return The block, or else NULL.
 */
internal void* restrict os_mem_map(unsigned int size, slice_prot_t prot) {

  void* restrict out =
    mmap(NULL, size, prot_map[prot], MAP_PRIVATE | MAP_ANON, -1, 0);

  PRINTD("mmap(NULL, %u, 0x%x, 0x%x, -1, 0) = %p\n", size,
	 prot_map[prot], MAP_PRIVATE | MAP_ANON, out);

  return (void*)-1 != out ? out : NULL;

}


/*!
 * This function unmaps a block of memory, removing it entirely from
 * the address space and releasing its memory.
 *
 * \brief Unmap a block of memory.
 * \arg ptr Pointer to the block.
 * \arg size Size of the block.
 */
internal void os_mem_unmap(void* restrict ptr, unsigned int size) {

  munmap(ptr, size);

}


/*!
 * This function alters the protections on a block of memory.  Note
 * that some operating systems may not support all possible
 * protections.
 *
 * \brief Alter the protections on a block of memory.
 * \arg ptr Pointer to the block.
 * \arg size Size of the block.
 * \arg prot The new protections for the block.
 */
internal void os_mem_remap(void* restrict ptr, unsigned int size,
			   slice_prot_t prot) {

  mprotect(ptr, size, prot_map[prot]);

}


/*!
 * This function informs the kernel that the requested block is
 * immanently needed.
 *
 * Note: on some systems (notably standalone systems), this does
 * nothing at all.
 *
 * \brief Mark block as immanently needed.
 * \arg ptr Pointer to the block.
 * \arg size Size of the block.
 */
internal void os_mem_willneed(void* restrict ptr, unsigned int size) {

  madvise(ptr, size, MADV_WILLNEED);

}


/*!
 * This function informs the kernel that the requested block is not
 * needed in the immediate future.  This preserves the contents of the
 * block.
 *
 * Note: on some systems (notably standalone systems), this does
 * nothing at all.
 *
 * \brief Mark block as not needed.
 * \arg ptr Pointer to the block.
 * \arg size Size of the block.
 */
internal void os_mem_dontneed(void* restrict ptr, unsigned int size) {

  madvise(ptr, size, MADV_DONTNEED);

}


/*!
 * This function informs the kernel that the requested block is not
 * needed, and the contents are disposable.  Unlike os_mem_dontneed,
 * this destroys the contents of the block.
 *
 * Note: on some systems (notably standalone systems), this does
 * nothing at all.  On most systems, this has the same effect as
 * os_mem_dontneed.
 *
 * \brief Mark block as expendable.
 * \arg ptr Pointer to the block.
 * \arg size Size of the block.
 */
internal void os_mem_release(void* restrict ptr, unsigned int size) {

  madvise(ptr, size, MADV_FREE);

}
