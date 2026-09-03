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
#define LOG_INTERVAL HZ
#define MSG_BUFSIZE 8

static DEFINE_MUTEX(ilog_mutex);
static dev_t devt;
static struct cdev interval_logger;
static struct class *cls;
static struct delayed_work work;
static unsigned long next_log;

char message_buf[MSG_BUFSIZE];
size_t message_size = 0;

/* 
 * ilog_work_handler()
 *
 * To counteract time drift the scheduling delay is calculated by subtracting
 * the current time from the ideal execution time of the next work item.
 *
 * If the actual time is already past this ideal execution time, the delay is
 * set to 0.
 */
static void ilog_work_handler(struct work_struct *work)
{
	struct delayed_work *dwork;
	unsigned long delay;

	mutex_lock(&ilog_mutex);
	pr_info("%.*s\n", (int)message_size, message_buf);
	mutex_unlock(&ilog_mutex);

	dwork = to_delayed_work(work);
	next_log += LOG_INTERVAL;
	delay = next_log - jiffies;
	schedule_delayed_work(dwork, delay > LOG_INTERVAL ? 0 : delay);
}

static void ilog_start_log(void)
{
	if (schedule_delayed_work(&work, LOG_INTERVAL)) {
		next_log = jiffies + LOG_INTERVAL;
		pr_info("scheduled work item\n");
	}
	else
		pr_info("work item already pending\n");
}

/*
 * ilog_open()
 */
static int ilog_open(struct inode *inode, struct file *filp)
{
	pr_debug("called\n");
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
			loff_t *f_pos)
{

	pr_debug("called\n");

	mutex_lock(&ilog_mutex);
	(void)cancel_delayed_work_sync(&work);
	message_size = 0;
	mutex_unlock(&ilog_mutex);

	return 0;
}

/*
 * ilog_write()
 *
 * ilog_start_log() needs to be protected to avoid this:
 *
 * Thread A in ilog_read()
 * Thread B in ilog_write()
 *
 * A: cancel_delayed_work_sync()
 * B: ilog_start_log()
 * A: message_size = 0;
 *
 * which would cause the logging of empty messages
 */
static ssize_t ilog_write(struct file *filp, const char __user *buf,
			 size_t count, loff_t *f_pos)
{
	size_t copy_size = min(count, MSG_BUFSIZE);

	pr_debug("called\n");

	mutex_lock(&ilog_mutex);
	if (copy_from_user(message_buf, buf, copy_size)) {
		mutex_unlock(&ilog_mutex);
		return -EFAULT;
	}
	message_size = copy_size;
	ilog_start_log();

	mutex_unlock(&ilog_mutex);

	return copy_size;
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
	pr_info("initializing %s ...\n", ILOG_DRIVER_NAME);

	int ret = 0;

	INIT_DELAYED_WORK(&work, ilog_work_handler);

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
	return ret;
}

static void __exit ilog_exit(void)
{
	pr_info("cleaning up %s ...\n", ILOG_DRIVER_NAME);

	device_destroy(cls, devt);
	class_destroy(cls);
	cdev_del(&interval_logger);
	unregister_chrdev_region(devt, 1);
	(void)cancel_delayed_work_sync(&work);

	pr_info("%s removed successfully\n", ILOG_DRIVER_NAME);
}

module_init(ilog_init);
module_exit(ilog_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("alneuma");
MODULE_DESCRIPTION("character device experiment using a workqueue");
