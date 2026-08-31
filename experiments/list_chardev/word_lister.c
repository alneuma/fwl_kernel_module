// This toy kernel module implements a character device that enqueues all words
// written to it in a list.
//
// A word is considered to be any number of consecutive bytes
// such that for each byte b holds: !isspace(b) or b == 0x0.
//
// Word boundaries are preserved between different calls to write().
// Different open file descriptions have independent word boundaries.
// Only per open file description completed words are added to the list.
//
// On release() the last uncompleted word is considered to be completed.
//
// Currently word size as well as list size are unbounded.
// Buffer size is limited to 4096.
// To prevent resource drain this will be changed in future iterations.
//
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
#define LCD_MAX_WRITE 4096
#define READ_WORD_SEP ' '

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

static int lcd_word_delim(char c)
{
	return isspace((unsigned char)c) || c == 0x0;
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

static size_t lcd_copy_word(const struct lcd_word *word, size_t pos, char *buf,
			    size_t buf_size)
{
	size_t copy_size = min(word->len - pos, buf_size);
	memcpy(buf, word->word + pos, copy_size);
	return copy_size;
}

static int nothing_to_read(struct lcd_file *file_data, loff_t pos)
{
	struct lcd_word *e;

	mutex_lock(&lcd_mutex);
	if (file_data->cur_read &&
	    list_is_last(file_data->cur_read, &word_list)) {
		e = container_of_const(file_data->cur_read, struct lcd_word,
				       node);
		if (pos == e->len) {
			mutex_unlock(&lcd_mutex);
			return 1;
		}
	}
	mutex_unlock(&lcd_mutex);
	return 0;
}


static ssize_t lcd_read(struct file *filp, char __user *buf, size_t count,
			loff_t *f_pos)
{
	pr_debug("called");

	struct lcd_file *file_data = filp->private_data;
	struct list_head *start = &word_list;
	struct lcd_word *e;
	struct list_head *anchor;
	loff_t pos;
	size_t copied_total = 0;
	size_t copied = 0;

	if (nothing_to_read(file_data, *f_pos))
		return 0;

	char *tmp_buf = kmalloc(count, GFP_KERNEL);
	if (!tmp_buf)
		return -ENOMEM;

	mutex_lock(&lcd_mutex);
	pos = *f_pos;
	anchor = file_data->cur_read;

	if (anchor)
		start = anchor;

	list_for_each_entry(e, start, node) {
		anchor = &e->node;
		if (copied_total == count)
			break;
		copied = lcd_copy_word(e, pos, tmp_buf + copied_total,
				       count - copied_total);
		copied_total += copied;
		pos += copied;
		if (copied_total == count)
			break;
		if (list_is_last(&e->node, &word_list))
			break;
		tmp_buf[copied_total++] = READ_WORD_SEP;
		pos = 0;
	}
	mutex_unlock(&lcd_mutex);

	if (copy_to_user(buf, tmp_buf, copied_total)) {
		kfree(tmp_buf);
		return -EFAULT;
	}
	kfree(tmp_buf);

	mutex_lock(&file_data->lock);
	*f_pos = pos;
	file_data->cur_read = anchor;
	mutex_unlock(&file_data->lock);

	return copied_total;
}

// lcd_word_make()
// composes a new word from prefix and suffix
//
// return values:
// success -> 0
// failure -> error < 0
//
// checked runtime errors:
// prefix_len + suffix_len == 0 -> -EINVAL
//
// unchecked runtime errors:
// prefix == NULL && prefix_len > 0
// suffix == NULL && suffix_len > 0
static int lcd_word_make(struct lcd_word **new_word, const char *prefix,
			 size_t prefix_len, const char *suffix,
			 size_t suffix_len)
{
	pr_debug("called");

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

// lcd_enlist_words()
//
// If successful words in buf will be appended to list. The struct lcd_word
// saved in filp->private_data will be considered to be the start of the buffer.
// if there is an unfinished word at the end of the buffer it will be saved
// in the struct lcd_word in filp->private_data.
//
// On failure filp and list will stay unmodified.
static int lcd_enlist_words(struct list_head *list, struct file *filp,
			    const char *buf, size_t buf_size)
{
	pr_debug("called");

	int ret = 0;
	size_t idx = 0;
	size_t wstart = 0;
	struct lcd_file *file_data = filp->private_data;
	struct lcd_word *new_word = NULL;

	mutex_lock(&file_data->lock);

	if (file_data->word) {
		while (idx < buf_size && !lcd_word_delim(buf[idx]))
			++idx;

		ret = lcd_word_make(&new_word, file_data->word->word,
				    file_data->word->len, buf, idx);
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
	kfree(file_data->word);
	file_data->word = new_word;
	mutex_unlock(&file_data->lock);
	return 0;
failure:
	mutex_unlock(&file_data->lock);
	lcd_word_list_clear(list);
	return ret;
}

static ssize_t lcd_write(struct file *filp, const char __user *buf,
			 size_t count, loff_t *f_pos)
{
	pr_debug("called");

	LIST_HEAD(tmp_list);
	int ret;

	if (!count)
		return 0;

	if (count > LCD_MAX_WRITE)
		return -E2BIG;

	char *devbuf = memdup_user(buf, count);
	if (IS_ERR(devbuf))
		return PTR_ERR(devbuf);

	ret = lcd_enlist_words(&tmp_list, filp, devbuf, count);
	kfree(devbuf);
	if (ret)
		return ret;

	mutex_lock(&lcd_mutex);
	list_splice_tail_init(&tmp_list, &word_list);
	mutex_unlock(&lcd_mutex);

	return count;
}

static int lcd_open(struct inode *inode, struct file *filp)
{
	pr_debug("called");

	// sets file->data->word = NULL
	struct lcd_file *file_data = kzalloc(sizeof(*file_data), GFP_KERNEL);
	if (!file_data)
		return -ENOMEM;

	mutex_init(&file_data->lock);
	filp->private_data = file_data;

	return 0;
}

// adds unfinished per open words to list
static int lcd_release(struct inode *inode, struct file *filp)
{
	pr_debug("called");

	struct lcd_file *file_data = filp->private_data;
	struct lcd_word *word = file_data->word;
	file_data->word = NULL;

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

	pr_info("initializing %s ...", LCD_DRIVER_NAME);

	ret = alloc_chrdev_region(&devt, 0, 1, LCD_DRIVER_NAME);
	if (ret) {
		pr_err("failed to initialize %s: alloc_chrdev_region(): %d",
		       LCD_DRIVER_NAME, ret);
		goto alloc_chrdev_region_failed;
	}

	cdev_init(&list_chardev, &lcd_ops);
	ret = cdev_add(&list_chardev, devt, 1);
	if (ret) {
		pr_err("failed to initialize %s: cdev_add(): %d",
		       LCD_DRIVER_NAME, ret);
		goto cdev_add_failed;
	}

	cls = class_create(LCD_DRIVER_NAME); // assumes kernel >= 6.4.0
	if (IS_ERR(cls)) {
		ret = PTR_ERR(cls);
		pr_err("failed to initialize %s: class_create(): %d",
		       LCD_DRIVER_NAME, ret);
		goto class_create_failed;
	}

	struct device *retp =
		device_create(cls, NULL, devt, NULL, LCD_DRIVER_NAME);
	if (IS_ERR(retp)) {
		ret = PTR_ERR(retp);
		pr_err("failed to initialize %s: device_create(): %d",
		       LCD_DRIVER_NAME, ret);
		goto device_create_failed;
	}

	pr_info("%s initialized successfully", LCD_DRIVER_NAME);
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
	pr_info("cleaning up %s ...", LCD_DRIVER_NAME);

	device_destroy(cls, devt);
	class_destroy(cls);
	cdev_del(&list_chardev);
	unregister_chrdev_region(devt, 1);
	lcd_word_list_clear(&word_list);

	pr_info("%s removed successfully", LCD_DRIVER_NAME);
}

module_init(lcd_init);
module_exit(lcd_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("alneuma");
MODULE_DESCRIPTION("character device experiment using a list");
