# Master's Thesis Code Archive

This is an archive of the code from my master's thesis.  It implements a
rudimentary runtime system consisting of a thread scheduler, a concurrent and
parallel garbage collector, and a malloc implementation based entirely on
lock-free algorithms.

Note that this code was written in 2007/2008, and I have not done any
appreciable work on it since.

The only occurrence of blocking synchronization of any kind occurs when
switching the garbage collector on and off, at which point a wait barrier is
executed, ensuring that all threads simultaneously acknowledge the change of
state.  This barrier is implemented with a mutex/condition variable pair,
as the last thread needs to do the work of switching the collector on or off.

Aside from this, there are no instances of blocking synchronization
*anywhere*; synchronization is achieved entirely through lock-free algorithms
and data structures.

## Design Notes

### Threading System

The threading system was designed around an M:N model, and ideally would
have made use of something akin to FreeBSD's KSE (kernel scheduling entity)
API, which was still viable back in 2008.  This in turn was built around a
model of upcalls in response to various system events, which made M:N
threading a viable model.  User threads were supposed to be very lightweight
entities that were mapped to scheduling entities called "executors".  Executors
would in turn use upcalls to switch between user threads and keep the
underlying CPU doing as much work as possible.

The scheduler's lock-free design proves particularly useful here, as all
scheduler operations are completely reentrant and deadlock-free.  Nothing
stops anyone from freely creating threads in the middle of signal handlers
for example- something that is typically impossible in lock-based schedulers.
I was intending to use this design to implement short-running interrupt
functions that magically turned themselves into threads if they ran too long.

At this point, however, KSE has been removed in FreeBSD, and no similar
system exists on any platform.  Moreover, CPU/memory topologies are of
much greater importance now, rendering this model somewhat antiquated.

Threading was built around a model of semi-voluntary context-switching using
safepoints to be inserted into the code by the compiler (this was intended to
be a runtime system for a functional programming language in the style of
Standard ML).  This facilitates much more efficient implementations of various
synchronization primitives as well as lightweight context-switching than would
be possible with involuntary context switching.

I ended up proving that one cannot efficiently implement synchronous channels
with single-word compare-and-set operations.  Despite my sincerest efforts,
I was utterly unable to publish that result.

### GC System

The GC algorithm is an adaptation of the Cheng-Blelloch collector which
uses only lock-free algorithms and data structures for tracing and copying.
It is a copying collector capable of performing collections concurrently with
regular program execution (a concurrent, parallel, copying collector).

The entire GC heap consists of large regions called "slices" (*never* design
a garbage collector around a contiguous memory space; this has caused the
hotspot JVM no end of trouble, not the least of which is the permgen).
Slices are allocated from the OS, meaning they involve blocking
synchronization.  For this reason, the scheduler does not use dynamic
allocation at all.

For the most part, the GC algorithm takes advatage of the fact that only the
source heap is being changed, and no writes are taking place in the destination
except to mirror the state of the source.

Interestingly, the GC system also has the ability to garbage-collect threads.
Threads are garbage-collected if they are suspended and not reachable (meaning
they can never be woken up).  This capability is also used to implement
finalizers.  A finalizer is a special kind of thread that is woken up instead
of being garbage-collected when it becomes non-reachable.  Finalizable
objects have an extra suffix which contains a reference to a finalizer thread;
when the object becomes unreachable, so does the finalizer thread, and it
is then woken up and begins to execute.

### Unmanaged Allocator

The unmanaged allocator is a realization of Maged Michael's lock-free malloc.
It uses the same underlying slice allocation system as the GC.

## Miscellaneous Notes

There are a couple of things that bear mentitoning:

* At the time, I had an icc (Intel C Compiler) license, and was rather
  obsessed with facilitating compiler optimizations, particularly
  whole-program optimizations.

* LLVM was in its infancy back then; it would have made targeting an actual
  language at this runtime system actually possible.  Instead, I had to
  manually craft tests out of C code.

* The build system uses ant, using a plugin for C compilation.  At the time,
  I loathed Makefiles and cmake was relatively unknown.
