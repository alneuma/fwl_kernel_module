# Fifo Word Logger
*a first Linux kernel module, tested with Linux kernel version 6.12.105*

Time spent: ~130h including research and setting up the development environment.
Use of AI: find learning resources, discuss kernel concepts, explore the Linux kernel API, generate code reviews without providing solutions
related prior experience: user space C and basic Linux administration

This implements a small character device driver that reads words into a queue and afterwards logs them to the kernel ring buffer, first in, first out.
A lot of care has been spend to precisely define its semantics and guarantee correctness under edge cases.

All the testing happened in a Debian 13 VM using a freshly compiled kernel with many debugging features enabled.

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

read() should return some representation of the current content of the queue. A simple solution is to construct a stream of queue entries separated by FWL_WORD_SEP.
if FWL_WORD_SEP is a space, then a queue of
```
Hello -- how -- are -- you?
```
Should write
```
"Hello how are you?"
```
to the read buffer.

#### Keeping track of read position

Because there is no guarantee that the whole stream can be passed with one single call to read(). Because of this per OFD state needs to be saved to keep track of the position of cursor position between calls to read(). This becomes more complicated once we take into account that between reads new words can be appended with write() or, even worse, words from the beginning of the queue can be removed during logging.

### Logging

When the queue's state switches from *empty* to *non-empty*, one second later, the first word of the queue is logged and removed from the queue. From then on logging repeats every second. This stops, once the last word is removed from the queue. We can think of two separate states:

*queue is empty*        -> no logging happens
*queue is non-empty*    -> logging and dequeuing words happens once a second

#### Interaction with read()

Let's imagine a scenario where we have a non-empty queue:
```
Hello -- how -- are -- you?
```
The following happens:
```
read of size 4 -> returns "Hell"
read of size 4 -> returns "o ho"
logging, removes "Hello"
logging, removes "how"
logging, removes "are"
read of size 4 -> ?
```
If no logging would have happened between reads and the queue was left in its original state, we would expect the final read to return `w ar`. But the removal of words has invalidated the read-cursor's position. `how` and `are` are no longer in the queue.

There are at least three ways to deal with this problem in a way that would semantically make sense:

1. Every node removed from the queue is kept in memory until there are no more references from any OFD's read-cursor to it. Read would go on as if the state of the queue was not changed, the words which are already removed form the queue would stay in memory until all OFD's that have started reading some of them, have finished reading all of them.
2. A variation of 1., where only currently pointed at words are kept in memory after and OFD has finished reading an already dequeued word, its read-cursor would jump to the beginning of the first word of the queue.
3. The cursor immediately jumps to the first word of the queue.

For this implementation I went with 3. If an OFD already started reading a removed word, the next read will prepend a separator to mark the beginning of a new word.
```
? = " you"
```

### Some less interesting details

#### error codes etc.

## Implementation

There are a couple of general considerations

1. Memory is limited
The amount of memory occupied by the device must be limited. We are in kernel space, unbound memory consumption can be really bad.
2. Concurrency is everywhere
Reads and writes from the same or from different OFDs can happen at any time. As long as the queue is not empty logging and word dequeuing can always interfere with those.
3. Everything can fail
Pretty much the same as in user space, but it somehow feels more real.
4. There is no *libc*, we can not use system calls, we provide them. All the help at our disposal comes from the internal Linux kernel API.

### General locking scheme
I am using two read/write semaphores for managing shared state, as well as one mutex per OFD to protect per OFD state from concurrent reads or writes. Here is an overview:

| lock order | name | function |
|-|-|-|
| 1 | `ofd_data.lock` | OFD private mutex protecting OFD state from concurrent accesses through the same OFD |
| 2 | `rw_sem_logging` | read/write semaphore protecting device wide shared data in situations where the only disrupting interfearance to readers could come from the word logging and dequeuing mechanism |
| 3 | `rw_sem_user` | read/write semaphore protecting all accesses to device wide shared data. It is called `*_user` because these accesses typically happen when data is transferred from or to user space. An exception to this is when the first word gets dequeued during a logging event. |

Not taking into account data, that is exclusively used by `fwl_init()` and `fwl_exit()`, the following device wide shared variables exist:

| name | function |
|-|-|
| `word_list` | the word queue |
| `mem_used` | persistent(!) dynamic memory occupied by the device, allocator overhead not taken into account |
| `node_idx_counter` | counter kept for of indexing queue nodes |
| `node_idx_num_reserved` | number of reserved queue node indices |

### custom types

| name | function |
|-|-|
| `struct fwl_word` | represents words as nodes of the queue |
| `struct fwl_cursor` | used by read() to represent a position inside of the stream constructed from the word queue |
| `struct fwl_ofd` | used to store per ofd state: `struct fwl_cursor`, a pointer to a `struct fwl_word` for pending unterminated words from prior writes and a mutex to protect this state from concurrent accesses |
| `struct fwl_transaction_write` | this is used to prepare and validate data received by write() before it is committed to the device's persistent state |

### Transactions
When a write happens, data flows from user space to the device. Many things can go wrong. Before any changes are committed to the device's persistent state. A transaction object is prepared and validated. If any non-recoverable error happens, the transaction object is dropped and the device's persistent state stays untouched. If the transaction is safe to be committed this happens in one atomic event protected by `rw_sem_user`.

```C
/* the transaction type */
struct fwl_transaction_write {
	struct list_head words;
	struct fwl_word *stash;
	size_t bytes_copied;
};

/* the most relevant functions */
static int fwl_transaction_populate_locked();
static int fwl_transaction_commit_locked();
```

The write function stack-allocates a transaction object. Together with the user provided buffer, and a possible pending unfinished word from the OFD's private data, this object is then passed to `fwl_transaction_populate_locked()`. Here the whole transaction gets prepared: A word list (`words`) gets created together with a potentially new pending unfinished word (`stash`). If this succeeds, write will call `fwl_transaction_commit_locked()`. Here checks for node index and memory exhaustion are performed. If they succeed the relating shared state are updated, before the indexed list segment is appended to the word queue and the OFD's old `stash` gets replaced by the new one provided by the transaction.
Both `fwl_transaction_populate_locked()` and `fwl_transaction_commit_locked()` need to be protected by the OFD local mutex, while only for the latter `re_sem_user` needs to be locked for writing.

### Logging
### The read cursor and node indexing

For an OFD to keep track where in the word queue it has finished its last read operation, words in the queue are indexed and a `struct fwl_cursor` object is saved within the OFD's private data.

```C
struct fwl_cursor {
	struct list_head *ptr; /* points to the word node */
	size_t word_pos; /* remembers the position withing the word */
	u32 node_idx; /* saves the index of the node */
};
```

In most cases an OFD can just continue reading from the queue, where it left. `ptr` directly points to the word node and `word_pos` denotes the offset from the beginning of the word. Once we take into consideration during logging events word nodes can be removed from the beginning of the queue, this gets brittle. What if the exact node the OFD's cursor was pointing to gets removed in between reads? To determine whether a cursor is still valid its saved `node_idx` is compared against the index of the fist node in the queue. A monotonically increasing indexing scheme guarantees, that if the cursor's `node_idx` is smaller than the index of the first node, the node the cursor is pointing to is no longer valid. In this case the cursor can be advanced to the first node of the queue.

#### Things to note about the cursor implementation

There are at least two things worth noting:

##### A different implementation strategy without `ptr`

Restoring the read position works without the node pointer in the cursor struct. The index is enough. At each read, the queue can simply be traversed until the word with the right index is found. The traversal can be done with a maximum of `n / 2`, where `n` is the number of words in the queue: We can check if the saved index is closer to the one of the first node or the last node in the queue and then traverse from the cheapest direction (we do have a doubly linked list). Also the number of nodes is naturally capped by the device's memory limit. Performance differences might only be noticed when there is a large and fully used memory limit and many reads are issued with small buffer sizes.
An advantage of the index only approach is that there is one less pointer variable per OFD to occupy memory and maybe more importantly, one variable less whose state needs to be tracked.

##### Indices are finite

In the current implementation indices can not be repurposed. This means that there is the possibility of index exhaustion. The total amount words that can be enqueued during the lifetime of the device is capped. In practice this should never happen, as the frequency with which words can be enqueued is capped by the one second second logging interval, once the device's memory limit has been reached:
The node of a one byte word occupies `33 bytes` of memory. With a memory limit of `1 GB` this amounts to a maximum of `32537631` words that can be enqueued at the same time. Lets assume all of those get enqueued immediately after the device is loaded. From then on new words can only be enqueued with a frequency of one per second. If we reserve one number as a sentinel, `u32` provides us with a pool of `2^32 - 1 = 4294967295` indices. So there are `4294967295 - 32537631 = 4262429666` seconds, or roughly `135` years left until index exhaustion. If anybody decides to use make use of the device driver in this way `write()` will eventually return `-ENOSPC`.
There is a book keeping implication: Because a pending word (`stash`) is enqueued when the OFD that owns it is released. An index needs to be reserved for each pending word, if we do not ever want to run into `close()` returning `-ENOSPC`.

### Memory accounting

## The most challenging parts

### define the interrelation between logging and read() semantics
### Reasoning about concurrency and shared state changes caused by the logger

## Possible refinements for future iterations

### Add a limit to the word size
### Command line configuration during module loading
- word size limit
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
