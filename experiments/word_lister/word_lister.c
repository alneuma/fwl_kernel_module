/*
 * This toy kernel module implements a character device that enqueues all words
 * written to it in a list.
 *
 * A word is considered to be any number of consecutive bytes
 * such that for each byte b holds: !isspace(b) or b == 0x0 or WORD_SEP
 *
 * f_pos is unused for read() and write().
 *
 * When read from it, the device does not preserve the characters of the
 * original word boundaries. Instead all the words are separated by a single
 * WORD_SEP byte when read.
 * 
 * Word boundaries are preserved between different calls to write().
 * Different open file descriptions have independent word boundaries.
 * Only per open file description completed words are added to the list.
 * 
 * On release() the last uncompleted word is considered to be completed.
 * 
 * Currently word size as well as list size are unbounded.
 * To prevent resource drain this will be changed in future iterations.
 * 
 * Currently also arbitrarily large read and write buffers are allowed.
 * This will be changed or appropriately handled in future iterations.
 *
 * Design consideration:
 * Should read on empty list return EOF or do something else?
 * atm it return EOF
 * consider blocking/-EAGAIN for blocking/non-blocking reads
 *
 * mutex lock order:
 *
 * 1. ofd local data lock
 * 2. lcd_mutex
 *
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
static size_t lcd_read_from_pos(struct lcd_cursor *pos, char *buf,
				size_t count)
{
	struct lcd_word *e;
	size_t copy_size;
	size_t idx = 0;

	e = list_entry(pos->ptr, struct lcd_word, node);

	/* write first word */
	if (pos->word_pos < e->len) {
		copy_size = min(e->len - pos->word_pos, count);
		memcpy(buf, e->word + pos->word_pos, copy_size);
		idx += copy_size;
		pos->word_pos += copy_size;
	}
	if (idx == count)
		return idx;

	/* write separator */
	if (!list_is_last(pos->ptr, &word_list)) {
		buf[idx++] = WORD_SEP;
		pos->word_pos = 0;
		pos->ptr = pos->ptr->next;
	} else
		return idx;

	/* write remaining */
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
 * lcd_read()
 *
 * Returns words joined with the single byte WORD_SEP.
 */
static ssize_t lcd_read(struct file *filp, char __user *buf, size_t count,
			loff_t *f_pos) {

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

/*
 * lcd_word_delim()
 *
 * In case WORD_SEP is not within the set defined by isspace(),
 * c == WORD_SEP is separately checked.
 */
static int lcd_word_delim(char c)
{
	return isspace((unsigned char)c) || c == 0x0 || c == WORD_SEP;
}

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
 * If successful words in buf will be appended to list. The struct lcd_word
 * saved in filp->private_data will be considered to be the start of the buffer.
 * if there is an unfinished word at the end of the buffer it will be saved
 * in the struct lcd_word in filp->private_data.
 *
 * On failure filp and list will stay unmodified.
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

static int lcd_open(struct inode *inode, struct file *filp)
{
	pr_debug("called\n");

	/* sets file->data->word = NULL */
	struct lcd_file *ofd_data = kzalloc(sizeof(*ofd_data), GFP_KERNEL);
	if (!ofd_data)
		return -ENOMEM;

	mutex_init(&ofd_data->lock);
	filp->private_data = ofd_data;

	return 0;
}

/* adds unfinished per open words to list */
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
