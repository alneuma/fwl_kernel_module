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

#include <asm/errno.h>

#define DRIVER_NAME "list_chardev"
#define DEVBUF_SIZE 1024

struct lcd_word {
	struct list_head node;
	size_t len;
	char word[];
};

static int major;
static struct cdev list_dev;
static struct class *cls;
LIST_HEAD(word_list);
static char devbuf[DEVBUF_SIZE];

static void lcd_log_word(const struct lcd_word *word, const size_t num)
{
	// very unlikely to happen, but still
	const int log_len =
		(int)(word->len < (size_t)INT_MAX ? word->len : INT_MAX);
	printk("%s: node %lu: %.*s", DRIVER_NAME, num, log_len, word->word);
}

static void lcd_log_list(void)
{
	struct lcd_word *e;
	struct list_head *cur;
	size_t num = 0;
	list_for_each(cur, &word_list) {
		++num;
		e = list_entry(cur, struct lcd_word, node);
		lcd_log_word(e, num);
	}
}

static ssize_t list_dev_read(struct file *filp, char __user *buf, size_t count,
			     loff_t *f_pos)
{
	pr_debug("called\n");

	lcd_log_list();
	return 0;
}

// success -> 0
// failure -> error < 0
static int lcd_word_make(struct lcd_word **new_word, const char *prefix,
			 const size_t prefix_len, const char *suffix,
			 const size_t suffix_len)
{
	const size_t len = prefix_len + suffix_len;
	*new_word = NULL;
	if (len == 0 || (!prefix && !suffix))
		return 0;
	*new_word = kmalloc(sizeof(**new_word) + len, GFP_KERNEL);
	if (!new_word)
		return -ENOMEM;
	if (prefix)
		memcpy((*new_word)->word, prefix, prefix_len);
	if (suffix)
		memcpy((*new_word)->word + prefix_len, suffix, suffix_len);
	(*new_word)->len = len;
	return 0;
}

// save_words()
//
// must be within mutex protected region
//
static int save_words(struct file *filp, const char *buf, const size_t buf_size)
{
	size_t idx = 0;
	size_t wstart = 0;
	int ret = 0;
	struct lcd_word *new_word;
	struct lcd_word *prefix = (struct lcd_word *)filp->private_data;
	filp->private_data = NULL;

	while (idx < buf_size && !isspace(buf[idx])) {
		++idx;
	}
	ret = lcd_word_make(&new_word, prefix ? prefix->word : NULL,
			    prefix ? prefix->len : 0, buf, idx);
	kfree(prefix);
	if (ret)
		return ret;
	if (idx == buf_size) {
		filp->private_data = new_word;
		return ret;
	}
	if (new_word)
		list_add_tail(&new_word->node, &word_list);

	while (idx < buf_size) {
		new_word = NULL;
		while (idx < buf_size && isspace(buf[idx]))
			++idx;
		if (idx == buf_size)
			return ret;
		wstart = idx;
		while (idx < buf_size && !isspace(buf[idx]))
			++idx;
		ret = lcd_word_make(&new_word, buf + wstart, idx - wstart, NULL,
				    0);
		if (ret)
			return ret;
		if (idx == buf_size)
			break;
		list_add_tail(&new_word->node, &word_list);
	}
	filp->private_data = new_word;

	return ret;
}

static ssize_t list_dev_write(struct file *filp, const char __user *buf,
			      size_t count, loff_t *f_pos)
{
	size_t copy_size;
	int ret;

	pr_debug("called\n");

	copy_size = DEVBUF_SIZE < count ? DEVBUF_SIZE : count;

	// start of mutex(?) protection
	// should be one writer or any number of readers
	if (copy_from_user(devbuf, buf, copy_size))
		return -EFAULT;

	pr_debug("copied %lu bytes to buffer\n", copy_size);

	ret = save_words(filp, devbuf, copy_size);
	if (ret)
		return ret;
	// end of mutex(?) protection

	pr_debug("filp->private_data = %p\n", filp->private_data);

	return copy_size;
}

static int list_dev_open(struct inode *inode, struct file *filp)
{
	pr_debug("called\n");

	filp->private_data = NULL;

	return 0;
}

static int list_dev_release(struct inode *inode, struct file *filp)
{
	pr_debug("called\n");
	lcd_log_list();

	// start of mutex(?) protection
	// should be one writer or any number of readers
	list_add_tail(&((struct lcd_word *)(filp->private_data))->node,
		      &word_list);
	// end of mutex(?) protection

	filp->private_data = NULL;

	return 0;
}

static struct file_operations list_dev_ops = {
	.owner = THIS_MODULE,
	.open = list_dev_open,
	.release = list_dev_release,
	.read = list_dev_read,
	.write = list_dev_write,
};

static int __init list_dev_init(void)
{
	dev_t devt;
	int ret = 0;

	printk("initializing %s ...\n", DRIVER_NAME);

	ret = alloc_chrdev_region(&devt, 0, 1, DRIVER_NAME);
	if (ret) {
		printk("failed to initialize %s: alloc_chrdev_region(): %d",
		       DRIVER_NAME, ret);
		goto alloc_chrdev_region_failed;
	}

	major = MAJOR(devt);
	cdev_init(&list_dev, &list_dev_ops);
	ret = cdev_add(&list_dev, devt, 1);
	if (ret) {
		printk("failed to initialize %s: cdev_add(): %d", DRIVER_NAME,
		       ret);
		goto cdev_add_failed;
	}

	cls = class_create(DRIVER_NAME); // assumes kernel >= 6.4.0
	if (IS_ERR(cls)) {
		ret = PTR_ERR(cls);
		printk("failed to initialize %s: class_create(): %d",
		       DRIVER_NAME, ret);
		goto class_create_failed;
	}

	struct device *retp = device_create(cls, NULL, devt, NULL, DRIVER_NAME);
	if (IS_ERR(retp)) {
		ret = PTR_ERR(retp);
		printk("failed to initialize %s: device_create(): %d",
		       DRIVER_NAME, ret);
		goto device_create_failed;
	}

	printk("%s initialized successfully\n", DRIVER_NAME);
	return 0;

device_create_failed:
	class_destroy(cls);
class_create_failed:
	cdev_del(&list_dev);
cdev_add_failed:
	unregister_chrdev_region(devt, 1);
alloc_chrdev_region_failed:
	return ret;
}

static void __exit list_dev_exit(void)
{
	struct lcd_word *e;
	struct lcd_word *n;
	dev_t devt = MKDEV(major, 0);

	printk("cleaning up %s ...\n", DRIVER_NAME);
	device_destroy(cls, devt);
	class_destroy(cls);
	cdev_del(&list_dev);
	unregister_chrdev_region(devt, 1);

	list_for_each_entry_safe(e, n, &word_list, node) {
		list_del(&e->node);
		kfree(e);
	}

	printk("%s removed successfully\n", DRIVER_NAME);
}

module_init(list_dev_init);
module_exit(list_dev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("alneuma");
MODULE_DESCRIPTION("character device experiment using a list");
