/*
 * interval_logger
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
#include <linux/workqueue.h>

#define ILOG_DRIVER_NAME "interval_logger"

static DEFINE_MUTEX(ilog_mutex);
static dev_t devt;
static struct cdev interval_logger;
static struct class *cls;
static struct workqueue_struct *queue = NULL;
static struct delayed_work work;

static char *message = "Wazzup?";

static void work_handler(struct work_struct *data)
{
	pr_info("%s\n", message);
}

/*
 * ilog_open()
 */
static int ilog_open(struct inode *inode, struct file *filp)
{
	pr_debug("called\n");

	INIT_DELAYED_WORK(&work, work_handler);

	queue_delayed_work(queue, &work, HZ);

	return 0;
}

/*
 * ilog_release()
 */
static int ilog_release(struct inode *inode, struct file *filp)
{
	pr_debug("called\n");

	return 0;
}

/*
 * ilog_read()
 */
static ssize_t ilog_read(struct file *filp, char __user *buf, size_t count,
			loff_t *f_pos) {

	pr_debug("called\n");

	return 0;
}

/*
 * ilog_write()
 */
static ssize_t ilog_write(struct file *filp, const char __user *buf,
			 size_t count, loff_t *f_pos)
{
	pr_debug("called\n");
	return count;
}

static const struct file_operations ilog_ops = {
	.owner = THIS_MODULE,
	.open = ilog_open,
	.release = ilog_release,
	.read = ilog_read,
	.write = ilog_write,
};

static int __init ilog_init(void)
{
	int ret = 0;

	pr_info("initializing %s ...\n", ILOG_DRIVER_NAME);

	queue = alloc_workqueue("WZZUPPER", WQ_UNBOUND, 0);
	if (!queue) {
		pr_err("failed to initialize %s: alloc_workqueue()\n",
		       ILOG_DRIVER_NAME);
		goto alloc_workqueue_failed;
	}

	ret = alloc_chrdev_region(&devt, 0, 1, ILOG_DRIVER_NAME);
	if (ret) {
		pr_err("failed to initialize %s: alloc_chrdev_region(): %d\n",
		       ILOG_DRIVER_NAME, ret);
		goto alloc_chrdev_region_failed;
	}

	cdev_init(&interval_logger, &ilog_ops);
	ret = cdev_add(&interval_logger, devt, 1);
	if (ret) {
		pr_err("failed to initialize %s: cdev_add(): %d\n",
		       ILOG_DRIVER_NAME, ret);
		goto cdev_add_failed;
	}

	cls = class_create(ILOG_DRIVER_NAME); /* assumes kernel >= 6.4.0 */
	if (IS_ERR(cls)) {
		ret = PTR_ERR(cls);
		pr_err("failed to initialize %s: class_create(): %d\n",
		       ILOG_DRIVER_NAME, ret);
		goto class_create_failed;
	}

	struct device *retp =
		device_create(cls, NULL, devt, NULL, ILOG_DRIVER_NAME);
	if (IS_ERR(retp)) {
		ret = PTR_ERR(retp);
		pr_err("failed to initialize %s: device_create(): %d\n",
		       ILOG_DRIVER_NAME, ret);
		goto device_create_failed;
	}

	pr_info("%s initialized successfully\n", ILOG_DRIVER_NAME);
	return 0;

device_create_failed:
	class_destroy(cls);
class_create_failed:
	cdev_del(&interval_logger);
cdev_add_failed:
	unregister_chrdev_region(devt, 1);
alloc_chrdev_region_failed:
	flush_workqueue(queue);
	destroy_workqueue(queue);
alloc_workqueue_failed:
	return ret;
}

static void __exit ilog_exit(void)
{
	pr_info("cleaning up %s ...\n", ILOG_DRIVER_NAME);

	device_destroy(cls, devt);
	class_destroy(cls);
	cdev_del(&interval_logger);
	unregister_chrdev_region(devt, 1);

	pr_info("%s removed successfully\n", ILOG_DRIVER_NAME);
}

module_init(ilog_init);
module_exit(ilog_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("alneuma");
MODULE_DESCRIPTION("character device experiment using a workqueue");
