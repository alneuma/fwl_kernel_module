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
 * A separator is any byte, that is FWL_WORD_SEP, the zero byte or any byte
 * for which isspace() returns true.
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
 * call to read() is not committed to the queue, but instead saved as ofd private
 * data. When no ofd is released any remaining unfinished word is committed to
 * the queue.
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
 * node has a unique index. Indices are represented by an unsigned integer type
 * and are finite. Indexing starts at 1 and 0 is used as a sentinel value. If
 * the next index to assign would be 0. We know that the next_index variable has
 * wrapped around and the pool of indices is exhausted.
 *
 * *** logging semantics and implementation ***
 * 
 * A new logging sequence starts when the state of the queue switches from empty 
 * to non-empty. The first logging event of a new sequence happens one second
 * after the sequence started. During each logging event the first word in the
 * queue will be removed from the queue and logged. If it was the last word in
 * the queue the logging sequence stops. A logging sequence is implemented by a
 * self rescheduling delayed word item.
 *
 * Thoughts about concurrency:
 * So the two relevant events for controlling the logging are associated with
 * changes in the queue's state:
 *
 * (1) empty -> non-empty causes logging to start
 * (2) non-empty -> empty causes logging to stop
 *
 * Questions of concurrency are simplified by the following:
 * - Event (1) is entirely controlled by the fwl_write() and fwl_release(). If
 *   any of them causes (1) it will start a logging sequence
 * - Event (2) is entirely controlled by fwl_work_handler(), the callback
 *   function of the work item, which will not self reschedule if it causes (2)
 *   and thus stop the logging sequence.
 * - Each of those events is locked to happen atomically.
 *
 * This gives the following guarantees:
 * (a) When the queue is empty there is either no work item scheduled or the
 *     callback has reached a stage in which it can no longer cause event (1)
 *     and will not reschedule.
 * (b) When the queue is non-empty no thread is in a stage where it can attempt
 *     to start logging.
 * (c) Because of (a), (1) can only happen in a context in which no work item
 *     that could cause (2) is scheduled.
 * (d) Because of (b) fwl_work_handler() can only reschedule itself in a context 
 *     in which no thread could attempt to start logging.
 * (e) Because of the atomicity of (1) it is impossible that multiple threads
 *     are contesting for starting logging: When one thread exits the critical
 *     section during which it caused (1), the queue's state has already changed
 *     to non-empty. So no other thread will be contesting for starting the
 *     logging sequence.
 * (f) (c), (d) and (e) guarantee, that access to the shared variable
 *     fwl_next_log will always be uncontested.
 *
 * One imaginative edge cases:
 *
 * 1. Thread A causes (1) then leaves the critical section.
 * 2. A work item that was still pending causes (2).
 * 3. Thread B causes (1)
 * 4. Thread A and Thread B are contenting for starting logging.
 *
 * But (c) rules out 2., so this can never happen.
 *
 * *** Caveats ***
 *
 * - word length, list length, and memory occupied are unbound
 * - read() buffers are dynamically allocated in the size of the buffers passed
 *   from userspace.
 * - Partial reads are not properly dealt with. Still partial reads can happen.
 *   The user must know, that in such cases the read cursor is not advanced.
 *
 * Locks:
 *
 * ofd local lock
 * protects ofd local state from concurrent reads/writes
 * release() is excempt from this.
 *
 * fwl_mutex
 * protects module wide shared state
 *
 * lock ordering
 * 1. ofd local lock
 * 2. fwl_mutex
 */
#define pr_fmt(fmt) "%s: %s: " fmt, KBUILD_MODNAME, __func__

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/printk.h>
#include <linux/init.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/ctype.h>
#include <linux/mutex.h>
#include <linux/errno.h>
#include <linux/overflow.h>
#include <linux/types.h>

#define FWL_DRIVER_NAME "fifo_word_logger"
#define FWL_WORD_SEP ' '
#define FWL_LOG_INTERVAL HZ
#define FWL_MAX_BUF 1024
#define FWL_MAX_MEM 4096

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
	bool on_sep;
};

struct fwl_ofd {
	struct fwl_cursor pos;
	struct mutex lock;
	struct fwl_word *stash;
};

/* bytes are not used yet */
struct fwl_transaction_write {
	struct list_head words;
	struct fwl_word *stash;
	size_t bytes_copied;
};

static DEFINE_MUTEX(fwl_mutex);
static dev_t devt;
static struct cdev fifo_word_logger;
static struct class *cls;
static LIST_HEAD(word_list);
static unsigned long fwl_next_log;
static struct delayed_work fwl_work;
static u32 next_node_idx = 1;
static size_t mem_used = 0;

/*
 * debugging functions
 */
static void fwl_cursor_log(const struct fwl_cursor *c, const char *label);
static void fwl_list_log(const struct list_head *l, const char *label);

static size_t fwl_word_size(const struct fwl_word *word)
{
	return struct_size(word, word, word->len);
}

/*
 * fwl_consume_word()
 */
static bool fwl_consume_word(struct list_head *words)
{
	struct fwl_word *e;
	int len;
	bool done;

	mutex_lock(&fwl_mutex);

	e = list_first_entry_or_null(words, struct fwl_word, node);
	if (!e) {
		mutex_unlock(&fwl_mutex);
		return true;
	}

	list_del_init(&e->node);
	mem_used -= fwl_word_size(e);

	pr_debug("mem_used: %zu\n", mem_used);

	done = list_empty(&word_list);

	mutex_unlock(&fwl_mutex);

	len = (int)min(e->len, (size_t)INT_MAX);
	pr_info("%.*s\n", len, e->word);
	kfree(e);

	return done;
}

/*
 * fwl_schedule_work()
 */
static void fwl_schedule_work(struct work_struct *work)
{
	unsigned long delay;

	fwl_next_log += FWL_LOG_INTERVAL;
	if (time_before(fwl_next_log, jiffies))
		delay = 0;
	else
		delay = fwl_next_log - jiffies;

	(void)schedule_delayed_work(to_delayed_work(work), delay);
}

/* 
 * fwl_work_handler()
 *
 * To counteract timer drift the scheduling delay is calculated by subtracting
 * the current time from the ideal execution time of the next work item.
 *
 * In the exceptional case in which the current time is already past the ideal
 * execution time of the next item, the delay is instead set to 0.
 *
 * note:
 * time_before() uses signed arithmetic for wraparound safety. This works within
 * the limitations of the "half-range rule".
 */
static void fwl_work_handler(struct work_struct *work)
{
	bool done = fwl_consume_word(&word_list);

	if (!done)
		fwl_schedule_work(work);
}

/*
 * fwl_start_logging()
 *
 * contract
 * (1) Should only be called right after word_list switches from empty to
 * non-empty.
 * (2) Can not hold fwl_mutex while calling this
 *
 * See "logging semantics and implementation" in the top most comment for a
 * discussion on concurrency.
 */
static void fwl_start_logging(void)
{
	(void)cancel_delayed_work_sync(&fwl_work);
	fwl_next_log = jiffies + FWL_LOG_INTERVAL;
	(void)schedule_delayed_work(&fwl_work, FWL_LOG_INTERVAL);
}

/*
 * fwl_cursor_update()
 * assumption: word not an empty list
 */
static void fwl_cursor_update(struct fwl_cursor *pos, struct list_head *words)
{
	struct fwl_word *e = list_first_entry(words, struct fwl_word, node);

	if (!pos->ptr || pos->node_idx < e->idx) {
		pos->on_sep = false;
		if (pos->ptr && pos->word_pos != 0)
			pos->on_sep = true;
		pos->ptr = words;
		pos->word_pos = 0;
	}
}

/*
 * fwl_read_from_pos()
 *
 * assumes words not empty
 *
 * A separator is "owned" by the word that comes before it. This means, that the
 * cursor does only advance to the next word, when the current word's trailing
 * separator is written. So the during regular reading
 *
 * pos->on_sep, is logically equivalent pos->word_pos.
 *
 * This rule is violated, when the cursor is advanced because of pointing
 * to a no longer existing word. In this situation we get
 *
 * pos->on_sep && pos->word_pos == 0.
 *
 */
static size_t fwl_read_from_pos(struct fwl_cursor *pos, char *buf, size_t count,
				struct list_head *words)
{
	struct fwl_word *e;
	size_t copy_size;
	size_t idx = 0;

	fwl_cursor_update(pos, words);

	while (idx < count) {
		if (pos->on_sep) {
			if (pos->word_pos != 0 && list_is_last(pos->ptr, words))
				goto done;
			buf[idx++] = FWL_WORD_SEP;
			pos->ptr = pos->ptr->next;
			pos->word_pos = 0;
			pos->on_sep = false;
		} else if (pos->ptr == words)
			pos->ptr = pos->ptr->next;

		if (idx == count)
			goto done;

		e = list_entry(pos->ptr, struct fwl_word, node);

		copy_size = min(e->len - pos->word_pos, count - idx);
		memcpy(buf + idx, e->word + pos->word_pos, copy_size);
		pos->on_sep = false;
		pos->word_pos += copy_size;
		idx += copy_size;
		if (idx == count)
			goto done;

		pos->on_sep = true;
	}

done:
	e = list_entry(pos->ptr, struct fwl_word, node);
	if (pos->word_pos == e->len)
		pos->on_sep = true;
	pos->node_idx = e->idx;
	return idx;
}

/*
 * fwl_word_delim()
 *
 * c == FWL_WORD_SEP is separately checked, in case FWL_WORD_SEP is not
 * within the set defined by isspace().
 */
static bool fwl_word_delim(char c)
{
	return isspace((unsigned char)c) || c == 0x0 || c == FWL_WORD_SEP;
}

/*
 * fwl_word_list_clear()
 */
static void fwl_word_list_clear(struct list_head *list)
{
	struct fwl_word *e;
	struct fwl_word *n;

	list_for_each_entry_safe(e, n, list, node) {
		list_del(&e->node);
		kfree(e);
	}
}

/*
 * fwl_word_make()
 * composes a new word from prefix and suffix
 *
 * return values:
 * success -> 0
 * failure -> error < 0
 *
 * checked runtime errors:
 * prefix_len + suffix_len == 0 -> -EINVAL
 * 
 * unchecked runtime errors:
 * prefix == NULL && prefix_len > 0
 * suffix == NULL && suffix_len > 0
 */
static int fwl_word_make(struct fwl_word **new_word, const char *prefix,
			 size_t prefix_len, const char *suffix,
			 size_t suffix_len)
{
	size_t len;
	size_t size;

	pr_debug("called\n");

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

	memcpy((*new_word)->word, prefix, prefix_len);
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

	pr_debug("called\n");

	if (old_stash) {
		while (idx < buf_size && !fwl_word_delim(buf[idx]))
			++idx;

		ret = fwl_word_make(&new_word, old_stash->word, old_stash->len, buf, idx);

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

	INIT_LIST_HEAD(&trans->words);
	trans->stash = NULL;
	trans->bytes_copied = 0;

	while (true) {
		to_copy = min(buf_size, count - trans->bytes_copied);
		not_copied = copy_from_user(devbuf, buf + trans->bytes_copied,
					    to_copy);
		if (not_copied == buf_size) {
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
	fwl_word_list_clear(&trans->words);
done:
	return ret;
}

static int fwl_transaction_update_counters(struct fwl_transaction_write *trans,
					 struct fwl_ofd *ofd_data)
{
	struct fwl_word *e;
	size_t bytes = 0;
	u32 tmp_idx = next_node_idx;

	if (check_add_overflow(bytes, fwl_word_size(trans->stash), &bytes))
		return -EOVERFLOW;

	if (bytes > FWL_MAX_MEM)
		return -ENOSPC; /* consider letting this block */

	list_for_each_entry(e, &trans->words, node) {
		if (!tmp_idx)
			return -ENOSPC;
		if (check_add_overflow(bytes, fwl_word_size(e), &bytes))
			return -EOVERFLOW;
		e->idx = tmp_idx++;
	}

	if (bytes > FWL_MAX_MEM)
		return -ENOSPC; /* consider letting this block */

	if (check_sub_overflow(bytes, fwl_word_size(ofd_data->stash), &bytes))
		return -EOVERFLOW;

	if (check_add_overflow(mem_used, bytes, &bytes))
		return -EOVERFLOW;

	next_node_idx = tmp_idx;
	mem_used = bytes;

	return 0;
}

/*
 * fwl_transaction_commit()
 *
 * must hold both mutexes
 *
 * will fail when:
 * - memory exhaustion (not implemented yet)
 * - node index exhaustion
 */
static int fwl_transaction_commit_locked(struct fwl_transaction_write *trans,
					 struct fwl_ofd *ofd_data,
					 struct list_head *words,
					 bool *should_log, size_t *copied)
{
	int ret = 0;
	*should_log = false;

	ret = fwl_transaction_update_counters(trans, ofd_data);
	if (ret)
		return ret;

	if (list_empty(words) && !list_empty(&trans->words))
		*should_log = true;

	list_splice_tail_init(&trans->words, words);

	kfree(ofd_data->stash);
	ofd_data->stash = trans->stash;
	trans->stash = NULL;

	*copied = trans->bytes_copied;

	mutex_lock(&fwl_mutex);
	pr_debug("mem_used: %zu\n", mem_used);
	mutex_unlock(&fwl_mutex);

	return 0;
}

static void fwl_transaction_clear(struct fwl_transaction_write *trans)
{
	kfree(trans->stash);
	fwl_word_list_clear(&trans->words);
}

/*
 * fwl_open()
 * initialized per ofd data
 */
static int fwl_open(struct inode *inode, struct file *filp)
{
	struct fwl_ofd *ofd_data;
	size_t mem_tmp = 0;
	size_t ret = 0;

	pr_debug("called\n");

	ofd_data = kzalloc(sizeof(*ofd_data), GFP_KERNEL);
	if (!ofd_data)
		return -ENOMEM;

	mutex_init(&ofd_data->lock);
	filp->private_data = ofd_data;

	mutex_lock(&fwl_mutex);
	if (check_add_overflow(mem_used, sizeof(*ofd_data), &mem_tmp))
		ret = -EOVERFLOW;
	else if (mem_tmp > FWL_MAX_MEM)
		ret = -ENOSPC;
	else
		mem_used = mem_tmp;
	mutex_unlock(&fwl_mutex);

	if (ret)
		kfree(filp->private_data);

	return ret;
}

/*
 * fwl_release()
 * cleans up and commits any unfinished words from per ofd_data->stash to list
 */
static int fwl_release(struct inode *inode, struct file *filp)
{
	struct fwl_ofd *ofd_data = filp->private_data;
	struct fwl_word *stash = ofd_data->stash;
	ofd_data->stash = NULL;
	bool should_log = false;

	pr_debug("called\n");

	if (stash) {
		mutex_lock(&fwl_mutex);

		if (!next_node_idx) {
			mutex_unlock(&fwl_mutex);
			kfree(stash);
			kfree(filp->private_data);
			return -ENOSPC;
		}
		stash->idx = next_node_idx++;

		should_log = list_empty(&word_list);

		list_add_tail(&stash->node, &word_list);

		mutex_unlock(&fwl_mutex);
		if (should_log)
			fwl_start_logging();
	}

	kfree(filp->private_data);

	mutex_lock(&fwl_mutex);
	mem_used -= sizeof(struct fwl_ofd);
	mutex_unlock(&fwl_mutex);

	return 0;
}

/*
 * fwl_write()
 *
 * To prevent an edgecases that would introduce unintuitive word ordering,
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
 * will be appended to the shared list, which is inconsitent with the order of
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

	pr_debug("called\n");

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

	mutex_lock(&fwl_mutex);

	ret = fwl_transaction_commit_locked(&trans, ofd_data, &word_list,
					    &should_log, &copied);

	mutex_unlock(&fwl_mutex);
done:
	mutex_unlock(&ofd_data->lock);

	pr_debug("ret: %d\n", ret);

	if (ret)
		fwl_transaction_clear(&trans);

	if (should_log)
		fwl_start_logging();

	return ret ? ret : copied;
}

/*
 * fwl_read()
 *
 * Returns words joined with the single byte FWL_WORD_SEP.
 */
static ssize_t fwl_read(struct file *filp, char __user *buf, size_t count,
			loff_t *f_pos)
{
	struct fwl_cursor pos;
	char *tmp_buf;
	int ret = 0;
	size_t total_read;
	struct fwl_ofd *ofd_data = filp->private_data;

	(void)f_pos;

	pr_debug("called\n");

	if (!count)
		return 0;

	tmp_buf = kmalloc(count, GFP_KERNEL);
	if (!tmp_buf)
		return -ENOMEM;

	mutex_lock(&ofd_data->lock);

	pos = ofd_data->pos;

	mutex_lock(&fwl_mutex);

	if (list_empty(&word_list)) {
		mutex_unlock(&fwl_mutex);
		mutex_unlock(&ofd_data->lock);
		kfree(tmp_buf);
		return 0;
	}

	total_read = fwl_read_from_pos(&pos, tmp_buf, count, &word_list);

	mutex_unlock(&fwl_mutex);

	if (copy_to_user(buf, tmp_buf, total_read))
		ret = -EFAULT;

	if (!ret)
		ofd_data->pos = pos;

	mutex_unlock(&ofd_data->lock);

	kfree(tmp_buf);
	return ret ? ret : total_read;
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
	fwl_word_list_clear(&word_list);

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
static void fwl_cursor_log(const struct fwl_cursor *c, const char *label)
{
	pr_debug("%s:\n", label);
	pr_debug("ptr = %p\n", c->ptr);
	pr_debug("word_pos = %zu\n", c->word_pos);
	pr_debug("node_idx = %u\n", c->node_idx);
	pr_debug("on_sep = %d\n", c->on_sep);
}

static void fwl_list_log(const struct list_head *l, const char *label)
{
	struct fwl_word *e;

	pr_debug("%s:\n", label);
	list_for_each_entry(e, l, node) {
		pr_debug("node %u: %.*s\n", e->idx, (int)e->len, e->word);
	}
}
