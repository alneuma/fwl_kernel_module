# Fifo Word Logger
*a first Linux kernel module, tested with Linux kernel version 6.12.105*

Time spent: ~130h including research and setting up the development environment.
Use of AI: find learning resources, discuss kernel concepts, explore the Linux kernel API, generate code reviews without providing solutions
related prior experience: user space C and basic Linux administration

This implements a small character device driver that reads words into a queue and afterwards logs them to the kernel ring buffer, first in, first out.
A lot of care has been spend to precisely define its semantics and guarantee correctness under edge cases.
For development a Linux kernel has been newly compiled, with many debugging features enabled.

## The most challenging parts

### define the interrelation between logging and read() semantics
### Reasoning about concurrency and shared state changes caused by the logger

## Semantics

### Words

#### What is a word?

Words are created from write() buffers. A word is any number of bytes delimited by separators. A separator is any of the following: the zero byte, the FWL_WORD_SEP byte(a compile time constant), any byte in the set defined by the isspace() function. The first byte written through an OFD is considered to be to the right of a separator. The last byte written through an OFD is considered to be to the left of a separator. Currently FWL_WORD_SEP is defined to be a simple ASCII space. A rather long winded way of saying, that a word is pretty much what you would expect it to be.

#### Preserving word integrity between writes

lets imagine we have this:
```C
write(device_fd, "Hel", 3);
write(device_fd, "lo", 2);
close(device_fd);
```
What we want to get is one word `Hello` not two words `Hel` and `lo`.

now let's imagine that we have two threads with separately opened OFDs writing concurrently:
```C
/* Thread A */
int device_fd_a = open(...);
write(device_fd_a, "Hel", 3);
write(device_fd_a, "lo", 2);
close(device_fd_a);

/* Thread B */
int device_fd_b = open(...);
write(device_fd_b, "Goo", 3);
write(device_fd_b, "dbye", 4);
close(device_fd_b);
```
The only permissible outcome is two words `Hello` and `Goodbye` in any order. What is not permissible are abominations like `Heldbye` or `Goolo` or even much more terrible things like `loGoo` or `GooHel`.

All of this points us at the necessity of keeping per OFD state of unterminated words between writes.

#### Stream construction for read()
#### Keeping track of read position

### Logging
#### Interaction with read()
#### read() adjusting its position in the stream when position got invalidated

## Implementation

### Transactions
### per OFD state
### Logging
### Node indexing
### Memory accounting

## Major points still missing

### Command line configuration during module loading
- word limits
- logging interval
- memory limit
### More elaborate testing setup
### Reworked memory management and accounting
### poll() and blocking behavior when appropriate
### More systematic documentation of invariants and concurrency considerations

/*
 * fifo_word_logger
 *
 * A character device that reads words and appends them to a queue.
 * Once per second the oldest word is logged and removed from the queue.
 * Counting starts as soon as the queue moves from empty to non-empty.
 *
 * *** Interface ***
 *
 * write()
 *
 * Splits content of the buffer into words and appends these words to the queue.
 * A word is any sequence of bytes that is surrounded by separators.
 * A separator is any byte, that is FWL_WORD_SEP, the zero byte or any byte for
 * which isspace() returns true.
 * The first byte of the first call to write() from a newly created ofd (open
 * file description) is considered to be to the right of a separator.
 * Similarly the last byte written before an ofd is released is considered to be
 * to the left of a separator.
 *
 * Word integrity between different calls to write() issued through the same ofd
 * is preserved. So if, during the lifetime of an ofd, we assume two consecutive
 * writes of size 4:
 *
 * "Hell" and "o Yo"
 *
 * , then
 *
 * "Hello" -- "Yo"
 * 
 * will be appended to the list, not
 *
 * "Hell" -- "o" -- "Yo"
 *
 * This behavior is implemented saving per ofd state of unfinished word: Any
 * word that remains unfinished (i.e. without a separator to its right) after a
 * call to read() is not committed to the queue, but instead saved as ofd
 * private data. When no ofd is released any remaining unfinished word is
 * committed to the queue.
 *
 * read()
 *
 * Writes all the currently enqueued words to the read buffer, separated by
 * FWL_WORD_SEP. The read position is saved per ofd.
 * It can occur, that between two calls to read() this position becomes
 * invalidated. This happens if between the two calls the word this position
 * refers after the first call gets removed from the queue.
 * In this case reading continues at the new first word of the queue,
 * potentially prepended by FWL_WORD_SEP, to distinguish it from a partial
 * read of the now dequeued word.
 *
 * When there are currently no more words to read, then EOF gets returned,
 * although more words could be committed later.
 * We might want to implement polling later.
 *
 * *** node indexing ***
 * 
 * To determine if a read cursor still points at a valid word node, each word
 * node has a unique index. Indices are represented by an unsigned integer type,
 * are finite and can not be reused. Indexing starts at 1. If assigning a next
 * index during write() would overflow the index counter, -ENOSPC is returned.
 *
 * *** memory limits ***
 * 
 * There is a maximum number of bytes that is allowed to be occupied by the
 * persistent device state. Objects counted are:
 * - nodes of the queue
 * - per ofd private data
 *
 * *** logging semantics and implementation ***
 * 
 * A new logging sequence starts when the state of the queue switches from empty 
 * to non-empty. The first logging event is scheduled to happen one second after
 * this. The work item's callback function does then establish the next_log
 * variable which always represents the ideal next log time. This variable is
 * used to counteract timer drift from the moment on the first callback happens.
 *
 * Thoughts about concurrency:
 * So the two relevant events for controlling the logging are associated with
 * changes in the queue's state:
 *
 * (1) empty -> non-empty causes logging to start
 * (2) non-empty -> empty causes logging to stop
 *
 * - Event (1) is entirely controlled by fwl_write() and fwl_release().
 * - Event (2) is entirely controlled by the callback, fwl_work_handler().
 * - Each of those events is locked to happen atomically.
 * - When fwl_write() causes (1) it will schedule a work item.
 * - When fwl_release() causes (1) it will schedule a work item.
 * - When fwl_work_handler() causes (2) it will not reschedule itself
 *
 * This gives the following guarantees:
 *
 * (a) When the queue is empty there is either no work item scheduled or the
 *     callback is currently executing, has caused (2) and has already
 *     determined that it will not reschedule itself.
 *
 * (b) When the queue is non-empty and (1) did not happen recently, the self
 *     rescheduling logging mechanism is running and neither fwl_write() nor
 *     fwl_release() are trying to schedule work items.
 *
 * (c) When (1) happened recently, i.e. the function that caused it is
 *     still executing but has not yet scheduled a new work item, then no work
 *     item is scheduled yet and no other thread is bound to schedule one. This
 *     holds because (1) happened atomically, so for the callback the conditions
 *     under (a) still hold. Likewise, also because of the atomicity, no other
 *     fwl_write()/fwl_release() thread can have caused (1) and as such no other
 *     fwl_write()/fwl_release() thread will attempt scheduling.
 * 
 * All of this guarantees that the control of starting/stopping a periodic
 * logging sequence will happen uncontested.
 *
 * *** Caveats ***
 *
 * - word length and list length are unbound.
 * - use of persistent memory is bound, but does not take into account
 *   kmalloc()'s allocator overhead.
 * - use of transient memory is not bound.
 *
 * Locks:
 *
 * ofd local lock
 * protects ofd local state from concurrent reads/writes
 * release() is exempt from this, as it is only called when all other references
 * to an ofd are gone.
 *
 * rw_sem_logging
 * protects shared state access from logging during specific inopportune moments
 * e.g. while read() unlocks rw_sem_user for calling copy_to_user()
 *
 * rw_sem_user
 * protects module wide shared state
 *
 * lock ordering
 * 1. ofd local lock
 * 2. rw_sem_logging
 * 3. rw_sem_user
 */
// #define pr_fmt(fmt) "%s: %s: " fmt, KBUILD_MODNAME, __func__
