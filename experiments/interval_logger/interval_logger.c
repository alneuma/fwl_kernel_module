/*
 * interval_logger
 *
 * A character device.
 *
 * When written to starts periodically logging the first set number of bytes of
 * the write. Consequent writes change what is logged.
 *
 * A reading cancels the logging and returns EOF.
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
#define LOG_INTERVAL HZ
#define MSG_BUFSIZE 8

static DEFINE_MUTEX(ilog_mutex);
static dev_t devt;
static struct cdev interval_logger;
static struct class *cls;
static struct delayed_work work;
static unsigned long next_log;
static bool logging = false;
static char message_buf[MSG_BUFSIZE];
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
 * time_before() uses signed arithmetic to determine the result for
 * wraparound cases. The half-range rule holds.
 */
static void ilog_work_handler(struct work_struct *work)
{
	struct delayed_work *dwork;
	unsigned long delay;

	mutex_lock(&ilog_mutex);

	if (!logging) {
		mutex_unlock(&ilog_mutex);
		return;
	}

	pr_info("%*phC\n", (int)message_size, message_buf);

	dwork = to_delayed_work(work);

	next_log += LOG_INTERVAL;
	if (time_before(next_log, jiffies))
		delay = 0;
	else
		delay = next_log - jiffies;

	schedule_delayed_work(dwork, delay);

	mutex_unlock(&ilog_mutex);
}

/*
 * ilog_start_log_lock()
 *
 * must be mutex protected
 */
static void ilog_start_log_lock(void)
{
	if (schedule_delayed_work(&work, LOG_INTERVAL)) {
		next_log = jiffies + LOG_INTERVAL;
		pr_info("scheduled work item\n");
	}
	else
		pr_info("work item already pending\n");
	logging = true;
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
	char tmp_buf[MSG_BUFSIZE];

	pr_debug("called\n");

	if (!copy_size)
		return 0;

	if (copy_from_user(tmp_buf, buf, copy_size)) {
		mutex_unlock(&ilog_mutex);
		return -EFAULT;
	}

	mutex_lock(&ilog_mutex);
	memcpy(message_buf, tmp_buf, copy_size);
	message_size = copy_size;
	ilog_start_log_lock();
	mutex_unlock(&ilog_mutex);

	return copy_size;
}

/*
 * ilog_read()
 */
static ssize_t ilog_read(struct file *filp, char __user *buf, size_t count,
			loff_t *f_pos)
{

	pr_debug("called\n");

	mutex_lock(&ilog_mutex);
	logging = false;
	message_size = 0;
	mutex_unlock(&ilog_mutex);

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

	INIT_DELAYED_WORK(&work, ilog_work_handler);

	ret = alloc_chrdev_region(&devt, 0, 1, ILOG_DRIVER_NAME);
	if (ret) {
		pr_err("failed to initialize %s: alloc_chrdev_region(): %d\n",
		       ILOG_DRIVER_NAME, ret);
		goto err_alloc_chrdev_regiond;
	}

	cdev_init(&interval_logger, &ilog_ops);
	ret = cdev_add(&interval_logger, devt, 1);
	if (ret) {
		pr_err("failed to initialize %s: cdev_add(): %d\n",
		       ILOG_DRIVER_NAME, ret);
		goto err_cdev_addd;
	}

	cls = class_create(ILOG_DRIVER_NAME);
	if (IS_ERR(cls)) {
		ret = PTR_ERR(cls);
		pr_err("failed to initialize %s: class_create(): %d\n",
		       ILOG_DRIVER_NAME, ret);
		goto err_class_created;
	}

	dev_ptr = device_create(cls, NULL, devt, NULL, ILOG_DRIVER_NAME);
	if (IS_ERR(dev_ptr)) {
		ret = PTR_ERR(dev_ptr);
		pr_err("failed to initialize %s: device_create(): %d\n",
		       ILOG_DRIVER_NAME, ret);
		goto err_device_created;
	}

	pr_info("%s initialized successfully\n", ILOG_DRIVER_NAME);
	return 0;

err_device_created:
	class_destroy(cls);
err_class_created:
	cdev_del(&interval_logger);
err_cdev_addd:
	unregister_chrdev_region(devt, 1);
err_alloc_chrdev_regiond:
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
