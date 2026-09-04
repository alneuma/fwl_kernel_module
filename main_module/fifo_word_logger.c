/*
 * fifo_word_logger
 *
 * A character device that reads words and appends them to a queue.
 * Once per second the oldest word is logged and removed from the queue.
 * Counting starts as soon as the queue moves from empty to non-empty.
 *
 * Interface:
 *
 * write()
 * Splits content of the buffer into words and appends these words to the queue.
 * A word is any sequence of bytes that is surrounded by separators.
 * A separator is any byte, that is FWL_WORD_SEP, the zero byte or any byte
 * for which isspace() returns true.
 * The first byte of the first call to write() of a newly created ofd (open file
 * description) is considered to be to the right of a separator.
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
 * call to read() is not committed to the queue, but insted saved as ofd private
 * data. When na ofd is released any remaining unfinished word is commited to
 * the queue.
 *
 * read()
 * Writes all the currently enqueud words to the read buffer, separated by
 * FWL_WORD_SEP. The read position is saved per ofd.
 * It can occur, that between two calls to read() this position becomes
 * invalidated. This happens if between the two calls the word this position
 * refers after the first call gets removed from the queue.
 * In this case reading continues at the new first word of the queue,
 * potentially prepended by as FWL_WORD_SEP, to distinguish it from a partial
 * read of the now dequeued word.
 *
 * When there are currently no more words to read, then EOF gets returned,
 * although more words could be commited later.
 * We might want to implement polling here.
 *
 * Caveats:
 *
 * - Once a word is commited to the list, it will stay there forever.
 * - word length, list length, and memory occupied are unbound
 * - read() and write() buffers are dynamically allocated in the size of the
 *   buffers passed from userspace.
 * - no partial reads or writes are properly dealt with. Still partial reads
 *   can happen. The user must know, that in such cases the read cursor is not
 *   advanced.
 *
 * Locks:
 *
 * ofd local lock
 * protects ofd local state from concurrent reads/writes
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
#define FWL_NODE_IDX u32

struct fwl_word {
	struct list_head node;
	size_t len;
	FWL_NODE_IDX idx;
	char word[];
};

struct fwl_cursor {
	struct list_head *ptr;
	size_t word_pos;
	FWL_NODE_IDX node_idx;
	bool on_sep;
};

struct fwl_file {
	struct fwl_cursor pos;
	struct mutex lock;
	struct fwl_word *stash;
};

/* bytes are not used yet */
struct fwl_transaction_write {
	struct list_head words;
	struct fwl_word *stash;
	size_t bytes;
};

static DEFINE_MUTEX(fwl_mutex);
static dev_t devt;
static struct cdev fifo_word_logger;
static struct class *cls;
static LIST_HEAD(word_list);

static void fwl_pos_update(struct fwl_cursor *pos)
{
	if (!pos->ptr) {
		pos->ptr = word_list.next;
		pos->word_pos = 0;
	}
	return;
}

static void fwl_cursor_set(struct fwl_cursor *pos)
{
	if (pos->node_idx < list_first->idx) {
		pos->ptr = list_head;
		if (pos->word_pos != 0)
			pos->on_sep = true;
	}
}


static size_t fwl_read_from_pos(struct fwl_cursor *pos, char *buf, size_t count)
{
	struct fwl_word *e;
	size_t copy_size;
	size_t idx = 0;
	
	// outside

	// on start
	if (pos->on_sep) {
		buf[idx++] = FWL_WORD_SEP;
		pos->ptr = pos->ptr->next;
		pos->on_sep = false;
		pos->word_pos = 0;
	}
	if (idx == count)
		goto done;
	// write first potentially partial word
	if (pos->word_pos < e->len) {
		copy_size = min(e->len - pos->word_pos, count);
		memcpy(buf, e->word + pos->word_pos, copy_size);
		idx += copy_size;
		pos->word_pos += copy_size;
		pos->on_sep = true;
	}
	
	// write remaining words
	while (idx < count) {
		e = list_entry(pos->ptr, struct fwl_word, node);
		copy_size = min(e->len, count - idx);
		memcpy(buf + idx, e->word + pos->word_pos, copy_size);
		idx += copy_size;
		pos->word_pos = copy_size;
		if (idx == count)
			return idx;
		if (!list_is_last(pos->ptr, &word_list)) {
			buf[idx++] = FWL_WORD_SEP;
			pos->word_pos = 0;
			pos->ptr = pos->ptr->next;
		} else
			return idx;
	}

done:
	if (pos->word_pos == cur_word->len)
		pos->on_sep = true;
	return idx;
}

/*
 * fwl_read_from_pos()
 *
 * assumptions:
 * - fwl_mutex held
 * - word_list not empty
 * - pos->ptr != NULL && !(pos->ptr == &word_list)
 */
static size_t fwl_read_from_pos_old(struct fwl_cursor *pos, char *buf, size_t count)
{
	struct fwl_word *e;
	size_t copy_size;
	size_t idx = 0;

	e = list_entry(pos->ptr, struct fwl_word, node);

	/* copy first word */
	if (pos->word_pos < e->len) {
		copy_size = min(e->len - pos->word_pos, count);
		memcpy(buf, e->word + pos->word_pos, copy_size);
		idx += copy_size;
		pos->word_pos += copy_size;
	}
	if (idx == count)
		return idx;

	/* copy separator */
	if (!list_is_last(pos->ptr, &word_list)) {
		buf[idx++] = FWL_WORD_SEP;
		pos->word_pos = 0;
		pos->ptr = pos->ptr->next;
	} else
		return idx;

	/* copy remaining */
	while (idx < count) {
		e = list_entry(pos->ptr, struct fwl_word, node);
		copy_size = min(e->len, count - idx);
		memcpy(buf + idx, e->word + pos->word_pos, copy_size);
		idx += copy_size;
		pos->word_pos = copy_size;
		if (idx == count)
			return idx;
		if (!list_is_last(pos->ptr, &word_list)) {
			buf[idx++] = FWL_WORD_SEP;
			pos->word_pos = 0;
			pos->ptr = pos->ptr->next;
		} else
			return idx;
	}

	return idx;
}

/*
 * fwl_word_delim()
 *
 * c == FWL_WORD_SEP is separately checked, in case FWL_WORD_SEP is not
 * within the set defined by isspace().
 */
static int fwl_word_delim(char c)
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
	pr_debug("called\n");

	size_t len;
	if (check_add_overflow(prefix_len, suffix_len, &len))
		return -EOVERFLOW;

	if (!len)
		return -EINVAL;

	const size_t size = struct_size(*new_word, word, len);
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
	pr_debug("called\n");

	int ret = 0;
	size_t idx = 0;
	size_t wstart = 0;
	struct fwl_word *new_word = NULL;

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
failure:
	return ret;
}

/*
 * fwl_transaction_clear()
 */
static void fwl_transaction_clear(struct fwl_transaction_write *trans)
{
	kfree(trans->stash);
	fwl_word_list_clear(&trans->words);
}

/*
 * fwl_transaction_commit()
 *
 * must hold both mutexes
 *
 * will fail when:
 * - memory exhaustion (not implemented yet)
 */
static int fwl_transaction_commit_locked(struct fwl_transaction_write *trans,
					 struct fwl_file *ofd_data,
					 struct list_head *word_list)
{
	static FWL_NODE_IDX next_idx = 1;
	FWL_NODE_IDX tmp_idx = next_idx;
	struct fwl_word *e;

	list_for_each_entry(e, &trans->words, node) {
		if (!tmp_idx)
			return -ENOSPC;
		e->idx = tmp_idx++;
	}
	next_idx = tmp_idx;

	list_splice_tail_init(&trans->words, word_list);

	kfree(ofd_data->stash);
	ofd_data->stash = trans->stash;
	trans->stash = NULL;

	return 0;
}

/*
 * fwl_open()
 * initialized per ofd data
 */
static int fwl_open(struct inode *inode, struct file *filp)
{
	pr_debug("called\n");

	struct fwl_file *ofd_data = kzalloc(sizeof(*ofd_data), GFP_KERNEL);
	if (!ofd_data)
		return -ENOMEM;

	mutex_init(&ofd_data->lock);
	filp->private_data = ofd_data;

	return 0;
}

/*
 * fwl_release()
 * cleans up and commits any unfinished words from per ofd_data->stash to list
 */
static int fwl_release(struct inode *inode, struct file *filp)
{
	pr_debug("called\n");

	struct fwl_file *ofd_data = filp->private_data;
	struct fwl_word *stash = ofd_data->stash;
	ofd_data->stash = NULL;

	if (stash) {
		mutex_lock(&fwl_mutex);
		list_add_tail(&stash->node, &word_list);
		mutex_unlock(&fwl_mutex);
	}

	kfree(filp->private_data);

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
	(void)f_pos;
	int ret = 0;
	struct fwl_file *ofd_data = filp->private_data;
	struct fwl_transaction_write trans = {
		.words = LIST_HEAD_INIT(trans.words), .stash = NULL, .bytes = 0
	};

	pr_debug("called\n");

	if (!count)
		goto zero_write;

	char *devbuf = memdup_user(buf, count);
	if (IS_ERR(devbuf)) {
		ret = PTR_ERR(devbuf);
		goto err_memdup_user;
	}

	mutex_lock(&ofd_data->lock);

	ret = fwl_transaction_update(&trans, ofd_data->stash, devbuf, count);
	kfree(devbuf);
	if (ret)
		goto err_fwl_transaction_update;

	mutex_lock(&fwl_mutex);
	ret = fwl_transaction_commit_locked(&trans, ofd_data, &word_list);
	mutex_unlock(&fwl_mutex);

err_fwl_transaction_update:
	mutex_unlock(&ofd_data->lock);
	fwl_transaction_clear(&trans);
err_memdup_user:
zero_write:
	return ret ? ret : count;
}

/*
 * fwl_read()
 *
 * Returns words joined with the single byte FWL_WORD_SEP.
 */
static ssize_t fwl_read(struct file *filp, char __user *buf, size_t count,
			loff_t *f_pos)
{
	(void)f_pos;

	struct fwl_cursor pos;
	char *tmp_buf;
	int ret = 0;
	size_t total_read;
	struct fwl_file *ofd_data = filp->private_data;

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

	fwl_pos_update(&pos);
	total_read = fwl_read_from_pos(&pos, tmp_buf, count);

	mutex_unlock(&fwl_mutex);

	if (copy_to_user(buf, tmp_buf, total_read))
		ret = -EFAULT;
	else
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
