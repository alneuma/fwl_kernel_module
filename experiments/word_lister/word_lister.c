/*
 * word_lister
 *
 * A character device
 *
 * write()
 * Splits content of buffer by WORD_SEP, the zero byte or any byte for which
 * isspace() returns true. The split segments (words) are saved to an internal
 * list.
 * Word integrity between different calls to write issued through the same
 * open file description is preserved. So if we assume two writes of size 4
 *
 * "Hell" and "o Yo"
 *
 * before the ofd is released. Then
 *
 * "Hello" -- "Yo"
 * 
 * will be appended to the list, not
 *
 * "Hell" -- "o" -- "Yo"
 *
 * This is achieved by saving per ofd state of unfinished words.
 * When an ofd is released a potential unfinished word is commited to the list.
 *
 * read()
 * Writes the content of the list's nodes separated by WORD_SEP into the buffer.
 * The read position is saved as per ofd state in the lcd_cursor struct.
 * When there are currently no more words to read, then EOF gets returned. But
 * more words could be commited later.
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
 * lcd_mutex
 * protects module wide shared state
 *
 * ofd local lock
 * protects ofd local state from concurrent reads/writes
 *
 * lock ordering
 * 1. ofd local lock
 * 2. lcd_mutex
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

#define LCD_DRIVER_NAME "word_lister"
#define WORD_SEP ' '

struct lcd_word {
	struct list_head node;
	size_t len;
	char word[];
};

struct lcd_cursor {
	struct list_head *ptr;
	size_t word_pos;
};

struct lcd_file {
	struct lcd_cursor pos;
	struct mutex lock;
	struct lcd_word *stash;
};

static DEFINE_MUTEX(lcd_mutex);
static dev_t devt;
static struct cdev list_chardev;
static struct class *cls;
static LIST_HEAD(word_list);

static void lcd_pos_update(struct lcd_cursor *pos)
{
	if (!pos->ptr) {
		pos->ptr = word_list.next;
		pos->word_pos = 0;
	}
	return;
}

/*
 * lcd_read_from_pos()
 *
 * assumptions:
 * - lcd_mutex held
 * - word_list not empty
 * - pos->ptr != NULL && !(pos->ptr == &word_list)
 */
static size_t lcd_read_from_pos(struct lcd_cursor *pos, char *buf, size_t count)
{
	struct lcd_word *e;
	size_t copy_size;
	size_t idx = 0;

	e = list_entry(pos->ptr, struct lcd_word, node);

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
		buf[idx++] = WORD_SEP;
		pos->word_pos = 0;
		pos->ptr = pos->ptr->next;
	} else
		return idx;

	/* copy remaining */
	while (idx < count) {
		e = list_entry(pos->ptr, struct lcd_word, node);
		copy_size = min(e->len, count - idx);
		memcpy(buf + idx, e->word + pos->word_pos, copy_size);
		idx += copy_size;
		pos->word_pos = copy_size;
		if (idx == count)
			return idx;
		if (!list_is_last(pos->ptr, &word_list)) {
			buf[idx++] = WORD_SEP;
			pos->word_pos = 0;
			pos->ptr = pos->ptr->next;
		} else
			return idx;
	}

	return idx;
}

/*
 * lcd_word_delim()
 *
 * c == WORD_SEP is separately checked, in case WORD_SEP is not within the set
 * defined by isspace().
 */
static int lcd_word_delim(char c)
{
	return isspace((unsigned char)c) || c == 0x0 || c == WORD_SEP;
}

/*
 * lcd_word_list_clear()
 */
static void lcd_word_list_clear(struct list_head *list)
{
	struct lcd_word *e;
	struct lcd_word *n;

	list_for_each_entry_safe(e, n, list, node) {
		list_del(&e->node);
		kfree(e);
	}
}

/*
 * lcd_word_make()
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
static int lcd_word_make(struct lcd_word **new_word, const char *prefix,
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
 * lcd_enlist_words()
 * 
 * Uses per ofd saved unfinished words from ofd_data->stash and the input buffer
 * to construct a list of words.
 *
 * On failure the whole list will be cleared and ofd_data->stash will stay
 * unmodified.
 *
 * On success ofd_data->stash will be either set to NULL or filled with a new
 * unfinished word.
 */
static int lcd_enlist_words(struct list_head *list, struct file *filp,
			    const char *buf, size_t buf_size)
{
	pr_debug("called\n");

	int ret = 0;
	size_t idx = 0;
	size_t wstart = 0;
	struct lcd_file *ofd_data = filp->private_data;
	struct lcd_word *new_word = NULL;

	if (ofd_data->stash) {
		while (idx < buf_size && !lcd_word_delim(buf[idx]))
			++idx;

		ret = lcd_word_make(&new_word, ofd_data->stash->word,
				    ofd_data->stash->len, buf, idx);
		if (ret)
			goto failure;

		if (idx == buf_size)
			goto success;

		list_add_tail(&new_word->node, list);
	}

	while (idx < buf_size) {
		new_word = NULL;
		while (idx < buf_size && lcd_word_delim(buf[idx]))
			++idx;
		if (idx == buf_size)
			goto success;

		wstart = idx;
		while (idx < buf_size && !lcd_word_delim(buf[idx]))
			++idx;

		ret = lcd_word_make(&new_word, buf + wstart, idx - wstart, NULL,
				    0);
		if (ret)
			goto failure;
		if (idx == buf_size)
			goto success;

		list_add_tail(&new_word->node, list);
	}

success:
	kfree(ofd_data->stash);
	ofd_data->stash = new_word;
	return 0;
failure:
	lcd_word_list_clear(list);
	return ret;
}

/*
 * lcd_open()
 * initialized per ofd data
 */
static int lcd_open(struct inode *inode, struct file *filp)
{
	pr_debug("called\n");

	struct lcd_file *ofd_data = kzalloc(sizeof(*ofd_data), GFP_KERNEL);
	if (!ofd_data)
		return -ENOMEM;

	mutex_init(&ofd_data->lock);
	filp->private_data = ofd_data;

	return 0;
}

/*
 * lcd_release()
 * cleans up and commits any unfinished words from per ofd_data->stash to list
 */
static int lcd_release(struct inode *inode, struct file *filp)
{
	pr_debug("called\n");

	struct lcd_file *ofd_data = filp->private_data;
	struct lcd_word *stash = ofd_data->stash;
	ofd_data->stash = NULL;

	if (stash) {
		mutex_lock(&lcd_mutex);
		list_add_tail(&stash->node, &word_list);
		mutex_unlock(&lcd_mutex);
	}

	kfree(filp->private_data);

	return 0;
}

/*
 * lcd_write()
 *
 * To prevent an edgecases that would introduce unintuitive word ordering,
 * ofd_data->lock is only released after the new list segment is commited to
 * the shared list.
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
 * If A executes lcd_enlist_words() before B but list_splice_tail_init() after
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
static ssize_t lcd_write(struct file *filp, const char __user *buf,
			 size_t count, loff_t *f_pos)
{
	(void)f_pos;

	pr_debug("called\n");

	LIST_HEAD(tmp_list);
	int ret;
	struct lcd_file *ofd_data = filp->private_data;

	if (!count)
		return 0;

	char *devbuf = memdup_user(buf, count);
	if (IS_ERR(devbuf))
		return PTR_ERR(devbuf);

	mutex_lock(&ofd_data->lock);

	ret = lcd_enlist_words(&tmp_list, filp, devbuf, count);
	kfree(devbuf);
	if (ret)
		return ret;

	mutex_lock(&lcd_mutex);

	list_splice_tail_init(&tmp_list, &word_list);

	mutex_unlock(&lcd_mutex);
	mutex_unlock(&ofd_data->lock);

	return count;
}

/*
 * lcd_read()
 *
 * Returns words joined with the single byte WORD_SEP.
 */
static ssize_t lcd_read(struct file *filp, char __user *buf, size_t count,
			loff_t *f_pos)
{
	(void)f_pos;

	struct lcd_cursor pos;
	char *tmp_buf;
	int ret = 0;
	size_t total_read;
	struct lcd_file *ofd_data = filp->private_data;

	pr_debug("called\n");

	if (!count)
		return 0;

	tmp_buf = kmalloc(count, GFP_KERNEL);
	if (!tmp_buf)
		return -ENOMEM;

	mutex_lock(&ofd_data->lock);

	pos = ofd_data->pos;

	mutex_lock(&lcd_mutex);

	if (list_empty(&word_list)) {
		mutex_unlock(&lcd_mutex);
		mutex_unlock(&ofd_data->lock);
		kfree(tmp_buf);
		return 0;
	}

	lcd_pos_update(&pos);
	total_read = lcd_read_from_pos(&pos, tmp_buf, count);

	mutex_unlock(&lcd_mutex);

	if (copy_to_user(buf, tmp_buf, total_read))
		ret = -EFAULT;
	else
		ofd_data->pos = pos;

	mutex_unlock(&ofd_data->lock);

	kfree(tmp_buf);
	return ret ? ret : total_read;
}

static const struct file_operations lcd_ops = {
	.owner = THIS_MODULE,
	.open = lcd_open,
	.release = lcd_release,
	.read = lcd_read,
	.write = lcd_write,
};

static int __init lcd_init(void)
{
	int ret = 0;
	struct device *dev_ptr;

	pr_info("initializing %s ...\n", LCD_DRIVER_NAME);

	ret = alloc_chrdev_region(&devt, 0, 1, LCD_DRIVER_NAME);
	if (ret) {
		pr_err("failed to initialize %s: alloc_chrdev_region(): %d\n",
		       LCD_DRIVER_NAME, ret);
		goto err_alloc_chrdev_region;
	}

	cdev_init(&list_chardev, &lcd_ops);
	ret = cdev_add(&list_chardev, devt, 1);
	if (ret) {
		pr_err("failed to initialize %s: cdev_add(): %d\n",
		       LCD_DRIVER_NAME, ret);
		goto err_cdev_add;
	}

	cls = class_create(LCD_DRIVER_NAME);
	if (IS_ERR(cls)) {
		ret = PTR_ERR(cls);
		pr_err("failed to initialize %s: class_create(): %d\n",
		       LCD_DRIVER_NAME, ret);
		goto err_class_create;
	}

	dev_ptr = device_create(cls, NULL, devt, NULL, LCD_DRIVER_NAME);
	if (IS_ERR(dev_ptr)) {
		ret = PTR_ERR(dev_ptr);
		pr_err("failed to initialize %s: device_create(): %d\n",
		       LCD_DRIVER_NAME, ret);
		goto err_device_create;
	}

	pr_info("%s initialized successfully\n", LCD_DRIVER_NAME);
	return 0;

err_device_create:
	class_destroy(cls);
err_class_create:
	cdev_del(&list_chardev);
err_cdev_add:
	unregister_chrdev_region(devt, 1);
err_alloc_chrdev_region:
	return ret;
}

static void __exit lcd_exit(void)
{
	pr_info("cleaning up %s ...\n", LCD_DRIVER_NAME);

	device_destroy(cls, devt);
	class_destroy(cls);
	cdev_del(&list_chardev);
	unregister_chrdev_region(devt, 1);
	lcd_word_list_clear(&word_list);

	pr_info("%s removed successfully\n", LCD_DRIVER_NAME);
}

module_init(lcd_init);
module_exit(lcd_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("alneuma");
MODULE_DESCRIPTION("character device experiment using a list");
