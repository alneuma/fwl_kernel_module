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

#include <asm/errno.h>

#define DRIVER_NAME "word_lister"

struct lcd_word {
	struct list_head node;
	size_t len;
	char word[];
};

static DEFINE_MUTEX(lcd_mutex);
static dev_t devt;
static struct cdev list_chardev;
static struct class *cls;
static LIST_HEAD(word_list);

static void lcd_log_word(const struct lcd_word *word, size_t num)
{
	pr_debug("called\n");

	// is this even possible?
	const int log_len =
		(int)(word->len < (size_t)INT_MAX ? word->len : INT_MAX);
	printk("node %zu: %.*s\n", num, log_len, word->word);
}

static void lcd_log_list(void)
{
	pr_debug("called\n");

	struct lcd_word *e;
	size_t num = 0;

	mutex_lock(&lcd_mutex);

	list_for_each_entry(e, &word_list, node) {
		lcd_log_word(e, ++num);
	}

	mutex_unlock(&lcd_mutex);
}

static ssize_t lcd_read(struct file *filp, char __user *buf, size_t count,
			     loff_t *f_pos)
{
	pr_debug("called\n");

	lcd_log_list();
	return 0;
}

// success -> 0
// failure -> error < 0
static int lcd_word_make(struct lcd_word **new_word, const char *prefix,
			 size_t prefix_len, const char *suffix,
			 size_t suffix_len)
{
	pr_debug("called\n");

	if (prefix_len > SIZE_MAX - suffix_len)
		return -EOVERFLOW;

	const size_t len = prefix_len + suffix_len;

	if (len > SIZE_MAX - sizeof(**new_word))
		return -EOVERFLOW;

	*new_word = NULL;
	if (len == 0)
		return 0;

	*new_word = kmalloc(sizeof(**new_word) + len, GFP_KERNEL);
	if (!*new_word)
		return -ENOMEM;

	if (prefix)
		memcpy((*new_word)->word, prefix, prefix_len);
	if (suffix)
		memcpy((*new_word)->word + prefix_len, suffix, suffix_len);

	(*new_word)->len = len;

	return 0;
}

// TODO: implement transaction semantics
// TODO: because list is local and does not get attached to shared one on
// failure, there are leaks on failure atm
static int lcd_enlist_words(struct list_head *lst, struct file *filp, const char *buf, size_t buf_size)
{
	pr_debug("called\n");

	size_t idx = 0;
	size_t wstart = 0;
	int ret = 0;
	struct lcd_word *new_word;
	struct lcd_word *prefix = filp->private_data;
	filp->private_data = NULL;

	while (idx < buf_size && !isspace((unsigned char)buf[idx])) {
		++idx;
	}
	ret = lcd_word_make(&new_word, prefix ? prefix->word : NULL,
			    prefix ? prefix->len : 0, buf, idx);
	if (ret)
		return ret;
	kfree(prefix);

	if (idx == buf_size) {
		filp->private_data = new_word;
		return ret;
	}
	if (new_word)
		list_add_tail(&new_word->node, lst);

	while (idx < buf_size) {
		new_word = NULL;
		while (idx < buf_size && isspace((unsigned char)buf[idx]))
			++idx;
		if (idx == buf_size)
			return ret;
		wstart = idx;
		while (idx < buf_size && !isspace((unsigned char)buf[idx]))
			++idx;
		ret = lcd_word_make(&new_word, buf + wstart, idx - wstart, NULL,
				    0);
		if (ret)
			return ret;
		if (idx == buf_size)
			break;
		list_add_tail(&new_word->node, lst);
	}
	filp->private_data = new_word;

	return ret;
}

static ssize_t lcd_write(struct file *filp, const char __user *buf,
			      size_t count, loff_t *f_pos)
{
	pr_debug("called\n");

	LIST_HEAD(tmp_list);
	int ret;

	char *devbuf = kmalloc(count, GFP_KERNEL);
	if (!devbuf) {
		ret = -ENOMEM;
		goto kmalloc_failed;
	}

	if (copy_from_user(devbuf, buf, count)) {
		ret = -EFAULT;
		goto copy_from_user_failed;
	}

	ret = lcd_enlist_words(&tmp_list, filp, devbuf, count);

	if (!ret) {
		mutex_lock(&lcd_mutex);
		list_splice_tail(&tmp_list, &word_list);
		mutex_unlock(&lcd_mutex);
	}

copy_from_user_failed:
	kfree(devbuf);
kmalloc_failed:
	return ret ? ret : count;
}

static int lcd_open(struct inode *inode, struct file *filp)
{
	pr_debug("called\n");

	filp->private_data = NULL;

	return 0;
}

static int lcd_release(struct inode *inode, struct file *filp)
{
	pr_debug("called\n");

	struct lcd_word *word = filp->private_data;

	if (word) {
		mutex_lock(&lcd_mutex);
		list_add_tail(&word->node, &word_list);
		mutex_unlock(&lcd_mutex);
		filp->private_data = NULL;
	}

	return 0;
}

static struct file_operations lcd_ops = {
	.owner = THIS_MODULE,
	.open = lcd_open,
	.release = lcd_release,
	.read = lcd_read,
	.write = lcd_write,
};

static int __init lcd_init(void)
{
	int ret = 0;

	printk("initializing %s ...\n", DRIVER_NAME);

	ret = alloc_chrdev_region(&devt, 0, 1, DRIVER_NAME);
	if (ret) {
		printk("failed to initialize %s: alloc_chrdev_region(): %d",
		       DRIVER_NAME, ret);
		goto alloc_chrdev_region_failed;
	}

	cdev_init(&list_chardev, &lcd_ops);
	ret = cdev_add(&list_chardev, devt, 1);
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
	cdev_del(&list_chardev);
cdev_add_failed:
	unregister_chrdev_region(devt, 1);
alloc_chrdev_region_failed:
	return ret;
}

static void __exit lcd_exit(void)
{
	struct lcd_word *e;
	struct lcd_word *n;

	printk("cleaning up %s ...\n", DRIVER_NAME);
	device_destroy(cls, devt);
	class_destroy(cls);
	cdev_del(&list_chardev);
	unregister_chrdev_region(devt, 1);

	mutex_lock(&lcd_mutex);
	list_for_each_entry_safe(e, n, &word_list, node) {
		list_del(&e->node);
		kfree(e);
	}
	mutex_unlock(&lcd_mutex);

	printk("%s removed successfully\n", DRIVER_NAME);
}

module_init(lcd_init);
module_exit(lcd_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("alneuma");
MODULE_DESCRIPTION("character device experiment using a list");
