/*
 * This toy kernel module implements a character device that enqueues all words
 * written to it in a list.
 *
 * A word is considered to be any number of consecutive bytes
 * such that for each byte b holds: !isspace(b) or b == 0x0 or WORD_SEP
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

#define LCD_DRIVER_NAME "word_lister"
#define WORD_SEP ' '

struct lcd_word {
	struct list_head node;
	size_t len;
	char word[];
};

struct lcd_file {
	struct mutex lock;
	struct lcd_word *word;
	struct list_head *cur_read;
};

static DEFINE_MUTEX(lcd_mutex);
static dev_t devt;
static struct cdev list_chardev;
static struct class *cls;
static LIST_HEAD(word_list);

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
 * lcd_read()
 *
 * returnes words joined with a single byte: WORD_SEP
 * *f_pos represents the position within this join
 *
 * Algorithm:
 * I traverse the virtually joined list of words with pos until I pos == *f_pos.
 * Then I start copying from the virtually joined list to the buffer while
 * increasing *f_pos.
 * I say "virtually joined" because while traversing I do not literally join the
 * word of the list except in the moment I copy them to the buffer.
 */
static ssize_t lcd_read(struct file *filp, char __user *buf, size_t count,
			loff_t *f_pos) {

	struct lcd_word *e;
	char *tmp_buf;
	loff_t f_pos_backup = *f_pos;
	loff_t pos;
	loff_t wrd_idx;
	loff_t buf_idx = 0;
	size_t copy_size;
	int ret = 0;

	pr_debug("called\n");

	if (!count)
		return 0;

	tmp_buf = kmalloc(count, GFP_KERNEL);
	if (!tmp_buf)
		return -ENOMEM;

	mutex_lock(&lcd_mutex);

	/* This check could potentially be moved before buffer allocation.
	 * But this would increase locking complexity.
	 */
	if (list_empty(&word_list))
		goto done;

	struct list_head *pos_lst = word_list.next;

	/* traverse the list until the word to which *f_pos belongs
	 *
	 * loop invariants:
	 * pos == amount of bytes before(!) the word at pos_list
	 *        if words were joined by a single byte
	 * pos < *f_pos
	 * pos_lst has not wrapped around
	 */
	pos = 0;
	while (pos_lst != &word_list) {
		e = list_entry(pos_lst, struct lcd_word, node);
		pos += e->len + 1;
		if (pos >= *f_pos)
			break;
		pos_lst = pos_lst->next;
	}
	if (pos_lst == &word_list) /* we have wrapped around */
		goto done;
	if (pos > *f_pos) { /* write first potentially partial word */
		wrd_idx = e->len + 1 - (pos - *f_pos);
		copy_size = min(e->len - wrd_idx, count - buf_idx);
		memcpy(tmp_buf + buf_idx, e->word + wrd_idx, copy_size);
		buf_idx += copy_size;
		*f_pos += copy_size;
	} 
	pos_lst = pos_lst->next;

	/* write remaining words
	 *
	 * loop invariants:
	 * *f_pos == amount of bytes before(!) the word at pos_list
	 *           if words were joined by a single byte
	 * buf_idx < count;
	 * pos_lst has not wrapped around
	 */
	while (buf_idx < count && pos_lst != &word_list) {
		tmp_buf[buf_idx++] = WORD_SEP;
		++*f_pos;
		if (buf_idx == count)
			goto done;
		e = list_entry(pos_lst, struct lcd_word, node);
		copy_size = min(e->len, count - buf_idx);
		memcpy(tmp_buf + buf_idx, e->word, copy_size);
		*f_pos += copy_size;
		buf_idx += copy_size;
		pos_lst = pos_lst->next;
	}

done:
	mutex_unlock(&lcd_mutex);

	if (copy_to_user(buf, tmp_buf, buf_idx)) {
		*f_pos = f_pos_backup;
		ret = -EFAULT;
	}
	kfree(tmp_buf);

	return ret ? ret : buf_idx;
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

	if (ofd_data->word) {
		while (idx < buf_size && !lcd_word_delim(buf[idx]))
			++idx;

		ret = lcd_word_make(&new_word, ofd_data->word->word,
				    ofd_data->word->len, buf, idx);
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
	kfree(ofd_data->word);
	ofd_data->word = new_word;
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
	struct lcd_word *word = ofd_data->word;
	ofd_data->word = NULL;

	if (word) {
		mutex_lock(&lcd_mutex);
		list_add_tail(&word->node, &word_list);
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

	pr_info("initializing %s ...\n", LCD_DRIVER_NAME);

	ret = alloc_chrdev_region(&devt, 0, 1, LCD_DRIVER_NAME);
	if (ret) {
		pr_err("failed to initialize %s: alloc_chrdev_region(): %d\n",
		       LCD_DRIVER_NAME, ret);
		goto alloc_chrdev_region_failed;
	}

	cdev_init(&list_chardev, &lcd_ops);
	ret = cdev_add(&list_chardev, devt, 1);
	if (ret) {
		pr_err("failed to initialize %s: cdev_add(): %d\n",
		       LCD_DRIVER_NAME, ret);
		goto cdev_add_failed;
	}

	cls = class_create(LCD_DRIVER_NAME); /* assumes kernel >= 6.4.0 */
	if (IS_ERR(cls)) {
		ret = PTR_ERR(cls);
		pr_err("failed to initialize %s: class_create(): %d\n",
		       LCD_DRIVER_NAME, ret);
		goto class_create_failed;
	}

	struct device *retp =
		device_create(cls, NULL, devt, NULL, LCD_DRIVER_NAME);
	if (IS_ERR(retp)) {
		ret = PTR_ERR(retp);
		pr_err("failed to initialize %s: device_create(): %d\n",
		       LCD_DRIVER_NAME, ret);
		goto device_create_failed;
	}

	pr_info("%s initialized successfully\n", LCD_DRIVER_NAME);
	return 0;

device_create_failed:
	class_destroy(cls);
class_create_failed:
	cdev_del(&list_chardev);
cdev_add_failed:
	unregister_chrdev_region(devt, 1);
alloc_chrdev_region_failed:
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
