#include <linux/module.h>
#include <linux/fs.h>
#include <linux/printk.h>
#include <linux/init.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
// #include <linux/list.h>

#include <asm/errno.h>

#define DRIVER_NAME "list_dev"
#define DEVBUF_SIZE 1024

// struct buf_node {
// 	size_t size;
// 	char *buf;
// 	struct list_head node;
// };

static int major;
static struct cdev list_dev;
static struct class *cls;
// struct buf_node buf_list;
static char devbuf[DEVBUF_SIZE];

static ssize_t list_dev_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
	printk("list_dev_read() called\n");
	//
	// if (*f_pos >= kbuf_size) {
	// 	*f_pos = 0;
	// 	return 0;
	// }
	//
	// unsigned long copy_size = min(kbuf_size - *f_pos, count);
	// if (copy_to_user(buf, kbuf, copy_size))
	// 	return -EFAULT;
	//
	// *f_pos += copy_size;
	//
	// return copy_size;
	return 0;
}

static int save_words(struct file *filp, const char *buf, const size_t buf_size)
{
	// save words to list
	// save last section that does not contain any whitespace to
	// filp->private_data
	return 0;
}

static ssize_t list_dev_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
	size_t copy_size;
	int ret;

	printk("list_dev_write() called\n");

	copy_size = DEVBUF_SIZE < count ? DEVBUF_SIZE : count;

	// needs to be mutex(?) protected
	if (copy_from_user(devbuf, buf, copy_size))
	 	return -EFAULT;

	ret = save_words(filp, buf, copy_size);
	if (ret)
		return ret;
	
	return copy_size;
}

static int list_dev_open(struct inode *inode, struct file *filp)
{
	printk("list_dev_open() called\n");
	return 0;
}

static int list_dev_release(struct inode *inode, struct file *filp)
{
	printk("list_dev_release() called\n");
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
		printk("failed to initialize %s: cdev_add(): %d",
		       DRIVER_NAME, ret);
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
