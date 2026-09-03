/*
 * interval_logger
 *
 * A character device
 *
 * write()
 * Causes periodic logging of the first set number of bytes written.
 * When the device is already logging, writes only change the message, but do
 * not reset the logging interval.
 * Writes of 0 have no effect
 *
 * read()
 * Reads of any size cause the logging to stop.
 * read() always returns EOF.
 */
#define pr_fmt(fmt) "%s: %s: " fmt, KBUILD_MODNAME, __func__

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/printk.h>
#include <linux/init.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/errno.h>
#include <linux/workqueue.h>

#define ILOG_DRIVER_NAME "interval_logger"
#define ILOG_LOG_INTERVAL HZ
#define ILOG_MSG_BUFSIZE 8

/*
 * Locks:
 *
 * ilog_control:
 * ascertains atomicity for operations controling the logging
 *
 * ilog_data:
 * protects concurrently read and written data
 *
 * lock ordering:
 * 1. ilog_control
 * 2. ilog_data
 */
static DEFINE_MUTEX(ilog_control);
static DEFINE_MUTEX(ilog_data);
static dev_t devt;
static struct cdev interval_logger;
static struct class *cls;
static struct delayed_work ilog_work;
static unsigned long next_log;
static char message_buf[ILOG_MSG_BUFSIZE];
static size_t message_size = 0;

/* 
 * ilog_work_handler()
 *
 * To counteract time drift the scheduling delay is calculated by subtracting
 * the current time from the ideal execution time of the next work item.
 *
 * If the actual time is already past this ideal execution time, the delay is
 * set to 0.
 *
 * note:
 * time_before() uses signed arithmetic for wraparound safety. This works within
 * the limitations of the half-range rule.
 */
static void ilog_work_handler(struct work_struct *work)
{
	struct delayed_work *dwork;
	unsigned long delay;

	mutex_lock(&ilog_data);

	if (!message_size) {
		mutex_unlock(&ilog_data);
		return;
	}

	pr_info("%*phC\n", (int)message_size, message_buf);

	mutex_unlock(&ilog_data);

	dwork = to_delayed_work(work);

	next_log += ILOG_LOG_INTERVAL;
	if (time_before(next_log, jiffies))
		delay = 0;
	else
		delay = next_log - jiffies;

	schedule_delayed_work(dwork, delay);
}

/*
 * ilog_schedule_work()
 */
static void ilog_schedule_work(void)
{
	if (schedule_delayed_work(&ilog_work, ILOG_LOG_INTERVAL)) {
		next_log = jiffies + ILOG_LOG_INTERVAL;
		pr_debug("scheduled work item\n");
	} else
		pr_debug("work item already pending\n");
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
 * ilog_write()
 *
 * write of 0 has no effect.
 *
 * Otherwise:
 *
 * If logging is active, this merely changes the message.
 * If no logging is active this sets the message and starts it.
 */
static ssize_t ilog_write(struct file *filp, const char __user *buf,
			  size_t count, loff_t *f_pos)
{
	size_t copy_size = min(count, ILOG_MSG_BUFSIZE);
	char tmp_buf[ILOG_MSG_BUFSIZE];

	pr_debug("called\n");

	if (!copy_size)
		return 0;

	if (copy_from_user(tmp_buf, buf, copy_size))
		return -EFAULT;

	mutex_lock(&ilog_control);
	mutex_lock(&ilog_data);

	memcpy(message_buf, tmp_buf, copy_size);
	message_size = copy_size;

	ilog_schedule_work();

	mutex_unlock(&ilog_data);
	mutex_unlock(&ilog_control);

	return copy_size;
}

/*
 * ilog_read()
 *
 * Stops logging, returns EOF
 *
 * cancel_delayed_work_sync() can not be protected by the same mutex that
 * is used by the work itself. Otherwise the is a deadlock scenario,
 * when cancel_delayed_work_sync() has acqured the lock before work:
 * work could be waiting for the lock while cancel_delayed_work_sync() is
 * waiting for work to finish.
 */
static ssize_t ilog_read(struct file *filp, char __user *buf, size_t count,
			 loff_t *f_pos)
{
	pr_debug("called\n");

	mutex_lock(&ilog_control);

	mutex_lock(&ilog_data);
	message_size = 0;
	mutex_unlock(&ilog_data);

	(void)cancel_delayed_work_sync(&ilog_work);
	mutex_unlock(&ilog_control);

	return 0;
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
	struct device *dev_ptr;

	pr_info("initializing %s ...\n", ILOG_DRIVER_NAME);

	INIT_DELAYED_WORK(&ilog_work, ilog_work_handler);

	ret = alloc_chrdev_region(&devt, 0, 1, ILOG_DRIVER_NAME);
	if (ret) {
		pr_err("failed to initialize %s: alloc_chrdev_region(): %d\n",
		       ILOG_DRIVER_NAME, ret);
		goto err_alloc_chrdev_region;
	}

	cdev_init(&interval_logger, &ilog_ops);
	ret = cdev_add(&interval_logger, devt, 1);
	if (ret) {
		pr_err("failed to initialize %s: cdev_add(): %d\n",
		       ILOG_DRIVER_NAME, ret);
		goto err_cdev_add;
	}

	cls = class_create(ILOG_DRIVER_NAME);
	if (IS_ERR(cls)) {
		ret = PTR_ERR(cls);
		pr_err("failed to initialize %s: class_create(): %d\n",
		       ILOG_DRIVER_NAME, ret);
		goto err_class_create;
	}

	dev_ptr = device_create(cls, NULL, devt, NULL, ILOG_DRIVER_NAME);
	if (IS_ERR(dev_ptr)) {
		ret = PTR_ERR(dev_ptr);
		pr_err("failed to initialize %s: device_create(): %d\n",
		       ILOG_DRIVER_NAME, ret);
		goto err_device_create;
	}

	pr_info("%s initialized successfully\n", ILOG_DRIVER_NAME);
	return 0;

err_device_create:
	class_destroy(cls);
err_class_create:
	cdev_del(&interval_logger);
err_cdev_add:
	unregister_chrdev_region(devt, 1);
err_alloc_chrdev_region:
	return ret;
}

/*
 * setting message_size = 0 potentially prevents an already running work item
 * from logging. This will hardly ever make an observable difference.
 */
static void __exit ilog_exit(void)
{
	pr_info("cleaning up %s ...\n", ILOG_DRIVER_NAME);

	mutex_lock(&ilog_data);
	message_size = 0;
	mutex_unlock(&ilog_data);

	(void)cancel_delayed_work_sync(&ilog_work);

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
