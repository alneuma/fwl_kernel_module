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
#define pr_fmt(fmt) "%s: " fmt, "fwl"

#include <linux/cdev.h>
#include <linux/ctype.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/printk.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#define FWL_DRIVER_NAME "fifo_word_logger"
#define FWL_WORD_SEP ' '
#define FWL_LOG_INTERVAL HZ
#define FWL_MAX_BUF 1024
#define FWL_MAX_MEM (1024 * 1024)

struct fwl_word {
	struct list_head node;
	size_t len;
	u32 idx;
	char word[];
};

struct fwl_cursor {
	struct list_head *ptr;
	size_t word_pos;
	u32 node_idx;
};

struct fwl_ofd {
	struct fwl_cursor pos;
	struct mutex lock;
	struct fwl_word *stash;
};

struct fwl_transaction_write {
	struct list_head words;
	struct fwl_word *stash;
	size_t bytes_copied;
};

static DECLARE_RWSEM(rw_sem_user);
static DECLARE_RWSEM(rw_sem_logging);
static dev_t devt;
static struct cdev fifo_word_logger;
static struct class *cls;
static LIST_HEAD(word_list);
static struct delayed_work fwl_work;
static u32 node_idx_counter = 0;
static u32 node_idx_num_reserved = 0;
static size_t mem_used = 0;

/*
 * debugging functions
 */
#ifdef DEBUG
static void fwl_counters_log(const char *label);
static void fwl_cursor_log(const struct fwl_cursor *c, const char *label);
static void fwl_list_log(const struct list_head *l, const char *label);
static void fwl_transaction_log(const struct fwl_transaction_write *t,
				const char *label);
#endif

static size_t fwl_word_size(const struct fwl_word *word)
{
	return struct_size(word, word, word->len);
}

/*
 * fwl_consume_first_word()
 * return
 * there is more to consume	-> true
 * otherwise			-> false
 *
 * The current scheduling model ascertains that fwl_consume_first_word() will
 * never be called when words is empty.
 */
static bool fwl_consume_first_word(struct list_head *words)
{
	struct fwl_word *e;
	int len;
	bool keep_going;

	down_write(&rw_sem_logging);
	down_write(&rw_sem_user);

	e = list_first_entry_or_null(words, struct fwl_word, node);
	if (!e) { /* this should never happen */
		up_write(&rw_sem_user);
		up_write(&rw_sem_logging);
		return false;
	}

	list_del_init(&e->node);
	mem_used -= fwl_word_size(e);
	keep_going = !list_empty(&word_list);

	up_write(&rw_sem_user);
	up_write(&rw_sem_logging);

	len = (int)min(e->len, (size_t)INT_MAX);
	pr_info("log %u: %.*s\n", len, e->word);
	kfree(e);

	return keep_going;
}

/*
 * fwl_schedule_work()
 *
 * In the exceptional case in which the current time is already past the ideal
 * execution time of the next item, delay is set to 0.
 *
 * note:
 * time_before() uses signed arithmetic for wraparound safety. This works within
 * the limitations of the "half-range rule".
 */
static void fwl_schedule_work(struct work_struct *work, unsigned long next_log)
{
	unsigned long delay;

	if (time_before(next_log, jiffies))
		delay = 0;
	else
		delay = next_log - jiffies;

	(void)schedule_delayed_work(to_delayed_work(work), delay);
}

/* 
 * fwl_work_handler()
 *
 * To counteract timer drift next_log is set relative to the previous one.
 */
static void fwl_work_handler(struct work_struct *work)
{
	static bool next_log_set = false;
	static unsigned long next_log = 0;

	if (!next_log_set) {
		next_log = jiffies + FWL_LOG_INTERVAL;
		next_log_set = true;
	} else
		next_log += FWL_LOG_INTERVAL;

	if (fwl_consume_first_word(&word_list))
		fwl_schedule_work(work, next_log);
	else
		next_log_set = false;
}

/*
 * fwl_word_delim()
 *
 * c == FWL_WORD_SEP is currently redundant, but still separately checked, in
 * case FWL_WORD_SEP is compiled with a replacement, that is not within the set
 * defined by isspace().
 */
static bool fwl_word_delim(char c)
{
	return isspace((unsigned char)c) || c == '\0' || c == FWL_WORD_SEP;
}

/*
 * fwl_word_list_clear()
 * returns amount of reclaimed dynamically allocated memory
 */
static size_t fwl_word_list_clear(struct list_head *list)
{
	struct fwl_word *e;
	struct fwl_word *n;
	size_t mem = 0;

	list_for_each_entry_safe(e, n, list, node) {
		mem += fwl_word_size(e);
		list_del(&e->node);
		kfree(e);
	}
	return mem;
}

/*
 * fwl_word_make()
 * composes a new word from prefix and suffix
 *
 * return values:
 * success -> 0
 * failure -> error < 0
 *
 * checked contract violations:
 * prefix_len + suffix_len == 0		-> -EINVAL
 * prefix == NULL && prefix_len > 0	-> -EINVAL
 * suffix == NULL && suffix_len > 0	-> -EINVAL
 */
static int fwl_word_make(struct fwl_word **new_word, const char *prefix,
			 size_t prefix_len, const char *suffix,
			 size_t suffix_len)
{
	size_t len;
	size_t size;

	//pr_debug("called\n");

	if (!prefix && prefix_len)
		return -EINVAL;
	if (!suffix && suffix_len)
		return -EINVAL;

	if (check_add_overflow(prefix_len, suffix_len, &len))
		return -EOVERFLOW;
	if (!len)
		return -EINVAL;

	size = struct_size(*new_word, word, len);
	if (size == SIZE_MAX)
		return -EOVERFLOW;

	*new_word = kmalloc(size, GFP_KERNEL);
	if (!*new_word)
		return -ENOMEM;

	if (prefix)
		memcpy((*new_word)->word, prefix, prefix_len);
	if (suffix)
		memcpy((*new_word)->word + prefix_len, suffix, suffix_len);

	(*new_word)->len = len;

	return 0;
}

/*
 * fwl_transaction_update()
 */
static int fwl_transaction_update(struct fwl_transaction_write *trans,
				  const struct fwl_word *old_stash,
				  const char *buf, size_t buf_size)
{
	int ret = 0;
	size_t idx = 0;
	size_t wstart = 0;
	struct fwl_word *new_word = NULL;

	//pr_debug("called\n");

	if (old_stash) {
		while (idx < buf_size && !fwl_word_delim(buf[idx]))
			++idx;

		ret = fwl_word_make(&new_word, old_stash->word, old_stash->len,
				    buf, idx);

		if (ret)
			goto failure;

		if (idx == buf_size)
			goto success;

		list_add_tail(&new_word->node, &trans->words);
	}

	while (idx < buf_size) {
		new_word = NULL;
		while (idx < buf_size && fwl_word_delim(buf[idx]))
			++idx;
		if (idx == buf_size)
			goto success;

		wstart = idx;
		while (idx < buf_size && !fwl_word_delim(buf[idx]))
			++idx;

		ret = fwl_word_make(&new_word, buf + wstart, idx - wstart, NULL,
				    0);
		if (ret)
			goto failure;

		if (idx == buf_size)
			goto success;

		list_add_tail(&new_word->node, &trans->words);
	}

success:
	trans->stash = new_word;
	trans->bytes_copied += buf_size;
failure:
	return ret;
}

/*
 * fwl_transaction_populate_locked()
 *
 * must hold ofd_data->lock
 */
static int fwl_transaction_populate_locked(struct fwl_transaction_write *trans,
					   struct fwl_ofd *ofd_data,
					   const char __user *buf, size_t count,
					   char *devbuf, size_t buf_size)
{
	size_t to_copy;
	int ret = 0;
	unsigned long not_copied;
	struct fwl_word *stash = ofd_data->stash;
	bool stash_owned = false;

	lockdep_assert_held(&ofd_data->lock);

	INIT_LIST_HEAD(&trans->words);
	trans->stash = NULL;
	trans->bytes_copied = 0;

	while (true) {
		to_copy = min(buf_size, count - trans->bytes_copied);
		not_copied = copy_from_user(devbuf, buf + trans->bytes_copied,
					    to_copy);
		if (not_copied == to_copy) {
			if (trans->bytes_copied == 0)
				ret = -EFAULT;
			goto done;
		}

		to_copy -= not_copied;
		ret = fwl_transaction_update(trans, stash, devbuf, to_copy);

		if (stash_owned)
			kfree(stash);

		if (ret)
			goto cleanup;

		if (not_copied || trans->bytes_copied == count)
			goto done;

		stash = trans->stash;
		trans->stash = NULL;
		stash_owned = true;
	}

cleanup:
	kfree(trans->stash);
	(void)fwl_word_list_clear(&trans->words);
done:
	return ret;
}

/*
 * fwl_node_idx_reserved_update()
 *
 * Subtraction would only overflow if node_idx_num_reserved == 0.
 * But this can not be the case if old_stash != NULL.
 */
static int fwl_node_idx_reserved_update(u32 *new_reserved, u32 old_reserved,
					const struct fwl_word *old_stash,
					const struct fwl_word *new_stash)
{
	u32 dummy;

	*new_reserved = old_reserved;
	if (old_stash && !new_stash)
		*new_reserved -= 1;
	else if (!old_stash && new_stash) {
		if (check_add_overflow(old_reserved, 1, new_reserved))
			return -ENOSPC;
	}

	if (check_add_overflow(*new_reserved, node_idx_counter, &dummy))
		return -ENOSPC;

	return 0;
}

/*
 * fwl_transaction_update_counters_locked()
 *
 * Assigns indices to transaction list and updates total memory usage.
 * On failure no shared state will be modified.
 *
 * Note:
 * Because every transaction already incorporates ofd_data->stash, the total
 * memory committed is always >= 0.
 *
 * will fail when:
 * - memory exhaustion
 * - node index exhaustion
 */
static int
fwl_transaction_update_counters_locked(struct fwl_transaction_write *trans,
				       struct fwl_ofd *ofd_data)
{
	struct fwl_word *e;
	size_t tmp_mem_used = 0;
	u32 tmp_idx = node_idx_counter;
	u32 tmp_idx_reserved;
	u32 dummy;

	lockdep_assert_held(&ofd_data->lock);
	lockdep_assert_held_write(&rw_sem_user);

	if (fwl_node_idx_reserved_update(&tmp_idx_reserved,
					 node_idx_num_reserved, ofd_data->stash,
					 trans->stash))
		return -ENOSPC;

	if (trans->stash)
		tmp_mem_used = fwl_word_size(trans->stash);

	list_for_each_entry(e, &trans->words, node) {
		if (check_add_overflow(tmp_idx, 1, &e->idx))
			return -ENOSPC;
		++tmp_idx;
		if (check_add_overflow(tmp_idx, tmp_idx_reserved, &dummy))
			return -ENOSPC;
		if (check_add_overflow(tmp_mem_used, fwl_word_size(e),
				       &tmp_mem_used))
			return -EOVERFLOW;
	}

	/* no need to check overflow, see note */
	if (ofd_data->stash)
		tmp_mem_used -= fwl_word_size(ofd_data->stash);

	if (check_add_overflow(mem_used, tmp_mem_used, &tmp_mem_used))
		return -EOVERFLOW;

	if (tmp_mem_used > FWL_MAX_MEM)
		return -ENOSPC;

	node_idx_num_reserved = tmp_idx_reserved;
	node_idx_counter = tmp_idx;
	mem_used = tmp_mem_used;

	return 0;
}

/*
 * fwl_transaction_commit()
 *
 * must hold ofd_data->lock and rw_sem_user
 *
 * will fail when:
 * - memory exhaustion
 * - node index exhaustion
 */
static int fwl_transaction_commit_locked(struct fwl_transaction_write *trans,
					 struct fwl_ofd *ofd_data,
					 struct list_head *words,
					 bool *should_log, size_t *copied)
{
	int ret = 0;
	*should_log = false;

	lockdep_assert_held(&ofd_data->lock);
	lockdep_assert_held(&rw_sem_user);

	ret = fwl_transaction_update_counters_locked(trans, ofd_data);

	if (ret)
		return ret;

	if (list_empty(words) && !list_empty(&trans->words))
		*should_log = true;

	list_splice_tail_init(&trans->words, words);

	kfree(ofd_data->stash);
	ofd_data->stash = trans->stash;
	trans->stash = NULL;

	*copied = trans->bytes_copied;

	return 0;
}

/*
 * fwl_transaction_clear()
 *
 * frees resources held by a transaction
 */
static void fwl_transaction_clear(struct fwl_transaction_write *trans)
{
	kfree(trans->stash);
	(void)fwl_word_list_clear(&trans->words);
}

/*
 * fwl_open()
 * initialized per ofd data
 */
static int fwl_open(struct inode *inode, struct file *filp)
{
	struct fwl_ofd *ofd_data;
	size_t mem_tmp = 0;
	int ret = 0;

	pr_debug("---- open() ----\n");

	ofd_data = kzalloc(sizeof(*ofd_data), GFP_KERNEL);
	if (!ofd_data)
		return -ENOMEM;

	down_write(&rw_sem_user);
	if (check_add_overflow(mem_used, sizeof(*ofd_data), &mem_tmp))
		ret = -EOVERFLOW;
	else if (mem_tmp > FWL_MAX_MEM)
		ret = -ENOSPC;
	else
		mem_used = mem_tmp;
	up_write(&rw_sem_user);

	if (!ret) {
		mutex_init(&ofd_data->lock);
		filp->private_data = ofd_data;
	} else
		kfree(ofd_data);

	return ret;
}

/*
 * fwl_release()
 * If there was a stash, a node index was reserved before.
 */
static int fwl_release(struct inode *inode, struct file *filp)
{
	struct fwl_ofd *ofd_data = filp->private_data;
	struct fwl_word *stash = ofd_data->stash;
	ofd_data->stash = NULL;
	bool should_log = false;

	//pr_debug("called\n");

	if (stash) {
		down_write(&rw_sem_user);

		stash->idx = ++node_idx_counter;
		--node_idx_num_reserved;

		should_log = list_empty(&word_list);

		list_add_tail(&stash->node, &word_list);
		stash = NULL;

		up_write(&rw_sem_user);

		if (should_log)
			(void)schedule_delayed_work(&fwl_work,
						    FWL_LOG_INTERVAL);
	}

	kfree(filp->private_data);

	down_write(&rw_sem_user);
	mem_used -= sizeof(struct fwl_ofd);
	up_write(&rw_sem_user);

	return 0;
}

/*
 * fwl_write()
 *
 * To prevent an edge cases that would introduce unintuitive word ordering,
 * ofd_data->lock is only released after the new list segment is commited to the
 * shared list.
 *
 * Consider this:
 *
 * Thread A and thread B share one open file description:
 *
 * A: writes "Hello W"
 * B: writes "orld "
 * A: calls close()
 * B: calls close()
 *
 * If A executes fwl_enlist_words() before B but list_splice_tail_init() after
 * B, the list
 *
 * "World" -- "Hello"
 *
 * will be appended to the shared list, which is inconsistent with the order of
 * bytes in A's write.
 *
 * With the mutex protection one of the following will be appended:
 *
 * 1. "Hello" -- "World"
 * 2. "orld" -- "Hello" -- "W"
 *
 * Both 1. and 2. are consistent with the byte ordering of individual writes.
 */
static ssize_t fwl_write(struct file *filp, const char __user *buf,
			 size_t count, loff_t *f_pos)
{
	size_t buf_size;
	size_t copied = 0;
	char *devbuf;
	struct fwl_ofd *ofd_data = filp->private_data;
	struct fwl_transaction_write trans;
	bool should_log = false;
	int ret = 0;

	//pr_debug("called\n");

	if (!count)
		return 0;

	buf_size = min(count, FWL_MAX_BUF);
	devbuf = kmalloc(buf_size, GFP_KERNEL);
	if (!devbuf)
		return -ENOMEM;

	mutex_lock(&ofd_data->lock);

	ret = fwl_transaction_populate_locked(&trans, ofd_data, buf, count,
					      devbuf, buf_size);

	kfree(devbuf);
	if (ret)
		goto done;

	down_write(&rw_sem_user);
	ret = fwl_transaction_commit_locked(&trans, ofd_data, &word_list,
					    &should_log, &copied);
	up_write(&rw_sem_user);

done:
	mutex_unlock(&ofd_data->lock);

	pr_debug("ret: %d\n", ret);

	if (ret)
		fwl_transaction_clear(&trans);
	else if (should_log)
		(void)schedule_delayed_work(&fwl_work, FWL_LOG_INTERVAL);

	return ret ? ret : copied;
}

/*
 * fwl_cursor_update()
 * a started word was dropped -> true
 * otherwise	   	      -> false
 *
 * assumption: word not an empty list
 */
static bool fwl_cursor_update(struct fwl_cursor *pos, struct list_head *words)
{
	struct fwl_word *e = list_first_entry(words, struct fwl_word, node);
	bool dropped = false;

	if (pos->node_idx < e->idx) {
		if (pos->word_pos != 0)
			dropped = true;
		pos->ptr = &e->node;
		pos->word_pos = 0;
		pos->node_idx = e->idx;
	}
	return dropped;
}

/*
 * fwl_cursor_advance_locked()
 *
 * must hold rw_sem_user for reading
 *
 * Advances pos, through the virtually continuous memory region constructed from
 * words, by count bytes or less if the region ends earlier.
 * When buf is not NULL, then the entirety of the traversed memory will be
 * copied to buf.
 *
 * A separator is "owned" by the word that comes before it. This means, that the
 * cursor does only advance to the next word, when the current word's trailing
 * separator is written.
 *
 */
static size_t fwl_cursor_advance_locked(struct fwl_cursor *pos, char *buf,
					size_t count, struct list_head *words)
{
	struct fwl_word *e;
	size_t copy_size;
	size_t idx = 0;

	/* can not assert ofd_data->lock held, as it is not accessible here */
	lockdep_assert_held_read(&rw_sem_user);

	if (list_empty(words))
		return 0;

	if (fwl_cursor_update(pos, words)) {
		if (buf)
			buf[idx] = FWL_WORD_SEP;
		if (++idx == count)
			goto done;
	}

	e = list_entry(pos->ptr, struct fwl_word, node);
	if (pos->word_pos == e->len) {
		if (list_is_last(pos->ptr, words))
			goto done;
		if (buf)
			buf[idx] = FWL_WORD_SEP;
		++idx;
		pos->ptr = pos->ptr->next;
		pos->word_pos = 0;
	}

	while (idx < count) {
		e = list_entry(pos->ptr, struct fwl_word, node);
		copy_size = min(e->len - pos->word_pos, count - idx);
		if (buf)
			memcpy(buf + idx, e->word + pos->word_pos, copy_size);
		pos->word_pos += copy_size;
		idx += copy_size;
		if (idx == count || list_is_last(pos->ptr, words))
			goto done;
		if (buf)
			buf[idx] = FWL_WORD_SEP;
		++idx;
		pos->ptr = pos->ptr->next;
		pos->word_pos = 0;
	}

done:
	e = list_entry(pos->ptr, struct fwl_word, node);
	pos->node_idx = e->idx;
	return idx;
}

/*
 * fwl_read_to_user_locked()
 *
 * must hold ofd_data->lock
 * 
 * Copies a maximum of count bytes from the virtually continuous memory region
 * that is constructed from word_list.
 * The start of the segment that is copied is indicated by pos.
 * After returning pos is updated to point to the first byte not copied.
 * devbuf is used as a temporary buffer in which the memory is constructed
 * first.
 *
 * We need to keep logging locked out during the call to copy_to_user(), to
 * prevent the following scenario:
 *
 * 1. pos is pointing to an invalid node 
 * 2. the first call to fwl_cursor_advance_locked() sets tmp_pos to the first
 * valid node and starts counting bytes from there.
 * 3. During the call to copy_to_user() logging happens and the first node gets
 * removed.
 * 4. copy_to_user() partially succeeds with a non zero return value
 * 5. cursor_advance_locked() with second argument NULL gets called, to adjust
 * pos to point to the correct node, but now the first valid node is different
 * than in 2. so an equal number of bytes does now represent a different offset
 * from the lists head. The cursor becomes corrupted.
 *
 * On the other hand calls to write() extending the list during the call to
 * copy_to_user() do not cause any trouble. Appending nodes, does not corrupt
 * the cursor.
 */
static ssize_t fwl_read_to_user_locked(struct fwl_cursor *pos, char __user *buf,
				       size_t count, char *devbuf,
				       size_t devbuf_size)
{
	/* can not assert ofd_data->lock held, as it is not accessible here */

	struct fwl_cursor tmp_pos = *pos;
	size_t not_copied;
	size_t bytes_read;
	size_t total_read = 0;
	int ret = 0;

	down_read(&rw_sem_logging);
	down_read(&rw_sem_user);

	while (total_read < count) {
		bytes_read = fwl_cursor_advance_locked(&tmp_pos, devbuf,
						       devbuf_size, &word_list);
		if (!bytes_read)
			goto done;

		up_read(&rw_sem_user);
		not_copied = copy_to_user(buf + total_read, devbuf, bytes_read);
		down_read(&rw_sem_user);

		total_read += bytes_read - not_copied;
		if (!total_read) {
			ret = -EFAULT;
			goto done;
		}
		if (not_copied) {
			fwl_cursor_advance_locked(
				pos, NULL, bytes_read - not_copied, &word_list);
			goto done;
		}
		*pos = tmp_pos;
	}

done:
	up_read(&rw_sem_user);
	up_read(&rw_sem_logging);
	return ret ? ret : total_read;
}

/*
 * fwl_read()
 *
 * Returns words joined with the single byte FWL_WORD_SEP.
 */
static ssize_t fwl_read(struct file *filp, char __user *buf, size_t count,
			loff_t *f_pos)
{
	char *devbuf = NULL;
	struct fwl_ofd *ofd_data = filp->private_data;
	size_t devbuf_size;
	bool empty_list = false;
	ssize_t ret = 0;

	(void)f_pos;

	//pr_debug("called\n");

	if (!count)
		return 0;

	/* this is relatively cheap and can prevent unnecessary allocations */
	down_read(&rw_sem_user);
	empty_list = list_empty(&word_list);
	up_read(&rw_sem_user);

	if (empty_list)
		return 0;

	devbuf_size = min(count, FWL_MAX_BUF);
	devbuf = kmalloc(devbuf_size, GFP_KERNEL);
	if (!devbuf)
		return -ENOMEM;

	mutex_lock(&ofd_data->lock);
	ret = fwl_read_to_user_locked(&ofd_data->pos, buf, count, devbuf,
				      devbuf_size);
	mutex_unlock(&ofd_data->lock);

	kfree(devbuf);

	return ret;
}

static const struct file_operations fwl_ops = {
	.owner = THIS_MODULE,
	.open = fwl_open,
	.release = fwl_release,
	.read = fwl_read,
	.write = fwl_write,
};

static int __init fwl_init(void)
{
	int ret = 0;
	struct device *dev_ptr;

	pr_info("initializing %s ...\n", FWL_DRIVER_NAME);

	INIT_DELAYED_WORK(&fwl_work, fwl_work_handler);

	ret = alloc_chrdev_region(&devt, 0, 1, FWL_DRIVER_NAME);
	if (ret) {
		pr_err("failed to initialize %s: alloc_chrdev_region(): %d\n",
		       FWL_DRIVER_NAME, ret);
		goto err_alloc_chrdev_region;
	}

	cdev_init(&fifo_word_logger, &fwl_ops);
	ret = cdev_add(&fifo_word_logger, devt, 1);
	if (ret) {
		pr_err("failed to initialize %s: cdev_add(): %d\n",
		       FWL_DRIVER_NAME, ret);
		goto err_cdev_add;
	}

	cls = class_create(FWL_DRIVER_NAME);
	if (IS_ERR(cls)) {
		ret = PTR_ERR(cls);
		pr_err("failed to initialize %s: class_create(): %d\n",
		       FWL_DRIVER_NAME, ret);
		goto err_class_create;
	}

	dev_ptr = device_create(cls, NULL, devt, NULL, FWL_DRIVER_NAME);
	if (IS_ERR(dev_ptr)) {
		ret = PTR_ERR(dev_ptr);
		pr_err("failed to initialize %s: device_create(): %d\n",
		       FWL_DRIVER_NAME, ret);
		goto err_device_create;
	}

	pr_info("%s initialized successfully\n", FWL_DRIVER_NAME);
	return 0;

err_device_create:
	class_destroy(cls);
err_class_create:
	cdev_del(&fifo_word_logger);
err_cdev_add:
	unregister_chrdev_region(devt, 1);
err_alloc_chrdev_region:
	return ret;
}

static void __exit fwl_exit(void)
{
	pr_info("cleaning up %s ...\n", FWL_DRIVER_NAME);

	(void)cancel_delayed_work_sync(&fwl_work);
	device_destroy(cls, devt);
	class_destroy(cls);
	cdev_del(&fifo_word_logger);
	unregister_chrdev_region(devt, 1);
	mem_used -= fwl_word_list_clear(&word_list);
	WARN_ON(mem_used);

	pr_info("%s removed successfully\n", FWL_DRIVER_NAME);
}

module_init(fwl_init);
module_exit(fwl_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("alneuma");
MODULE_DESCRIPTION("character device experiment using a list");

/*
 * debugging functions
 */
#ifdef DEBUG
static void fwl_counters_log(const char *label)
{
	pr_debug("%s\n", label);
	pr_debug("node_idx_counter = %u\n", node_idx_counter);
	pr_debug("node_idx_num_reserved = %u\n", node_idx_num_reserved);
	pr_debug("mem_used: %zu\n", mem_used);
}
static void fwl_transaction_log(const struct fwl_transaction_write *t,
				const char *label)
{
	pr_debug("%s\n", label);
	pr_debug("t->stash = %p\n", t->stash);
	pr_debug("t->bytes_copied = %zu\n", t->bytes_copied);
}
static void fwl_cursor_log(const struct fwl_cursor *c, const char *label)
{
	pr_debug("%s\n", label);
	pr_debug("ptr = %p\n", c->ptr);
	pr_debug("word_pos = %zu\n", c->word_pos);
	pr_debug("node_idx = %u\n", c->node_idx);
}
static void fwl_list_log(const struct list_head *l, const char *label)
{
	struct fwl_word *e;
	pr_debug("%s\n", label);
	list_for_each_entry(e, l, node) {
		pr_debug("node %u: %.*s\n", e->idx, (int)e->len, e->word);
	}
}
#endif
