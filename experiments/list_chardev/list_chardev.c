#include <linux/module.h>
#include <linux/fs.h>
#include <linux/printk.h>
#include <linux/init.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/list.h>

#include <asm/errno.h>

#define DRIVER_NAME "list_dev"
#define DEVBUF_SIZE 1024

struct lcd_word {
	size_t len;
	char word[];
};

struct lcd_word_node {
	struct list_head list;
	struct lcd_word word;
};

static int major;
static struct cdev list_dev;
static struct class *cls;
LIST_HEAD(word_list);
static char devbuf[DEVBUF_SIZE];

static ssize_t list_dev_read(struct file *filp, char __user *buf, size_t count,
			     loff_t *f_pos)
{
	printk("list_dev_read: called\n");

	struct lcd_word_node *e;
	struct list_head *cur;
	char sep = '\0';
	size_t num_nodes = 0;
	list_for_each(cur, &word_list) {
		++num_nodes;
		e = list_entry(cur, struct lcd_word_node, list);
		// unsafe cast to (int)
		printk("%c%.*s", sep, (int)e->word.len, e->word.word);
		sep = ' ';
	}
	printk("%lu nodes\n", num_nodes);
	return 0;
}

static int lcd_isspace(const char c)
{
	return c == ' ';
}

// lcd_append_word()
//
// must be within mutex protected region
//
// if prefix != NULL
// appends word with prefix
//
// if prefix == NULL
// appends word
//
// if word == NULL
// only appends prefix
//
// if word == NULL && prefix == NULL
// does nothing
static int lcd_append_word(const struct lcd_word *prefix, const char *word,
			   const size_t len)
{
	printk("lcd_append_word: called\n");
	size_t total_len;
	size_t idx;

	printk("lcd_append_word: prefix = %p\n", prefix);

	total_len = len;
	if (prefix)
		total_len += prefix->len;

	printk("lcd_append_word: total len = %lu\n", total_len);
	if (!total_len)
		return 0;

	struct lcd_word_node *new_node =
		kmalloc(sizeof(*new_node) + total_len, GFP_KERNEL);
	if (!new_node)
		return -ENOMEM;

	new_node->word.len = total_len;

	idx = 0;
	if (prefix) {
		memcpy(new_node->word.word, prefix->word, prefix->len);
		idx += prefix->len;
	}

	if (word)
		memcpy(new_node->word.word + idx, word, len);

	list_add_tail(&new_node->list, &word_list);

	printk("lcd_append_word: appended word\n");

	return 0;
}

static int lcd_save_residue(struct file *filp, const char *res,
			    const size_t len)
{
	struct lcd_word *stash = filp->private_data;

	if (stash == NULL) {
		stash = kmalloc(sizeof(*stash) + len, GFP_KERNEL);
		if (!stash)
			return -ENOMEM;

		memcpy(stash->word, res, len);
		stash->len = len;
		printk("lcd_save_residue: saved %ld bytes in file struct\n", len);
	} else {
		//edgecase: there still is residue
	}

	printk("lcd_save_residue: stash = %p\n", stash);
	printk("lcd_save_residue: stash->len = %lu\n", stash->len);
	printk("lcd_save_residue: stash->word = %.*s", (int)stash->len, stash->word);

	return 0;
}

// save_words()
//
// must be within mutex protected region
//
static int save_words(struct file *filp, const char *buf, const size_t buf_size)
{
	enum parse_state {
		WORD_RES,
		WORD_NO_RES,
		NO_WORD,
	};

	size_t idx = 0;
	size_t wstart = 0;
	int ret = 0;
	enum parse_state state = WORD_RES;
	int idx_space = lcd_isspace(buf[idx]);

	if (idx_space) {
		ret = lcd_append_word(filp->private_data, NULL, 0);
		kfree(filp->private_data);
		filp->private_data = NULL;
		if (ret)
			return ret;
		++idx;
		state = NO_WORD;
	}

	while (idx != buf_size) {
		if (state == WORD_RES && idx_space) {
			ret = lcd_append_word(filp->private_data,
					      buf + wstart,
					      idx - wstart);
			kfree(filp->private_data);
			filp->private_data = NULL;
			if (ret)
				break;
			state = NO_WORD;
		}
		else if (state == WORD_NO_RES && idx_space) {
			ret = lcd_append_word(filp->private_data,
					      buf + wstart,
					      idx - wstart);
			if (ret)
				break;
			state = NO_WORD;
		}
		else if (state == NO_WORD && !idx_space) {
			wstart = idx;
			state = WORD_NO_RES;
		}

		++idx;
		idx_space = lcd_isspace(buf[idx]);
	}

	if (state != NO_WORD)
		ret = lcd_save_residue(filp, buf + wstart, buf_size - wstart);
	struct lcd_word *stash = filp->private_data;
	printk("lcd_save_word: stash = %p\n", stash);
	printk("lcd_save_word: stash->len = %lu\n", stash->len);
	printk("lcd_save_word: stash->word = %.*s", (int)stash->len, stash->word);

	return ret;
}

static ssize_t list_dev_write(struct file *filp, const char __user *buf,
			      size_t count, loff_t *f_pos)
{
	size_t copy_size;
	int ret;

	printk("list_dev_write: called\n");

	copy_size = DEVBUF_SIZE < count ? DEVBUF_SIZE : count;

	// start of mutex(?) protection
	// should be one writer or any number of readers
	if (copy_from_user(devbuf, buf, copy_size))
		return -EFAULT;
	printk("list_dev_write: copied %lu bytes to buffer\n", copy_size);

	ret = save_words(filp, devbuf, copy_size);
	if (ret)
		return ret;
	// end of mutex(?) protection
	printk("list_dev_write: filp->private_data = %p\n", filp->private_data);

	return copy_size;
}

static int list_dev_open(struct inode *inode, struct file *filp)
{
	printk("list_dev_open: called\n");

	filp->private_data = NULL;

	return 0;
}

static int list_dev_release(struct inode *inode, struct file *filp)
{
	printk("list_dev_release: called\n");

	printk("lcd_append_word: filp->private_data = %p\n", filp->private_data);

	// start of mutex(?) protection
	// should be one writer or any number of readers
	if (lcd_append_word(filp->private_data, NULL, 0))
		return -ENOMEM;
	// end of mutex(?) protection

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

	// alloc_chrdev_region()
	// returns major number + first minor number in dev
	// second argument = base minor
	// third argument = count
	ret = alloc_chrdev_region(&devt, 0, 1, DRIVER_NAME);
	if (ret) {
		printk("failed to initialize %s: alloc_chrdev_region(): %d",
		       DRIVER_NAME, ret);
		goto alloc_chrdev_region_failed;
	}

	major = MAJOR(devt);
	cdev_init(&list_dev, &list_dev_ops);
	ret = cdev_add(&list_dev, devt, 1); // add one device
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
	dev_t devt = MKDEV(major, 0);

	printk("cleaning up %s ...\n", DRIVER_NAME);
	device_destroy(cls, devt);
	class_destroy(cls);
	cdev_del(&list_dev);
	unregister_chrdev_region(devt, 1);
	printk("%s removed successfully\n", DRIVER_NAME);
}

module_init(list_dev_init);
module_exit(list_dev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("alneuma");
MODULE_DESCRIPTION("character device experiment using a list");
