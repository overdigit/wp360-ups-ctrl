#include <linux/init.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/cdev.h>
#include <linux/printk.h>
#include <linux/gpio/consumer.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/syscalls.h>
#include <linux/time.h>
#include <asm/errno.h>
#include <linux/platform_device.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/atomic.h>

#include "wp360-pmuc-driver.h"

// Sysfs interface
static struct kobject mymodule;

#define N_ATTRIBUTES (sizeof(attributes)/sizeof(struct wp360_pmuc_sysfs_attribute))
static struct wp360_pmuc_sysfs_attribute attributes[] = {
	{"power_voltage_nominal", 105, 300, __ATTR(power_voltage_nominal, 0664, sysfs_show, sysfs_store)},
	{"power_voltage_min",     90,  300, __ATTR(power_voltage_min,     0664, sysfs_show, sysfs_store)},
	{"capacitor_voltage_min", 52,  140, __ATTR(capacitor_voltage_min, 0664, sysfs_show, sysfs_store)},
	{"switching_voltage_min", 50,  300, __ATTR(switching_voltage_min, 0664, sysfs_show, sysfs_store)},
	{"battery_voltage_min",   50,  140, __ATTR(battery_voltage_min,   0664, sysfs_show, sysfs_store)},
	{"battery_version",       0,     1, __ATTR(battery_version,       0664, sysfs_show, sysfs_store)},
	{"port_poweroff",         0,   255, __ATTR(port_poweroff,         0664, sysfs_show, sysfs_store)},
	{"switching_timeout",     0, 65535, __ATTR(switching_timeout,     0664, sysfs_show, sysfs_store)},
	{"power_voltage_measure", 0,     0, __ATTR(power_voltage,         0444, sysfs_show, sysfs_ronly)},
	{"capacitor_voltage_measure", 0, 0, __ATTR(capacitor_voltage,     0444, sysfs_show, sysfs_ronly)},
};

static struct attribute *wp360_pmuc_attrs[N_ATTRIBUTES + 1];
ATTRIBUTE_GROUPS(wp360_pmuc);

static const struct kobj_type wp360_pmuc_ktype = {
	.sysfs_ops = &kobj_sysfs_ops,
	.default_groups = wp360_pmuc_groups,
};

static ssize_t sysfs_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	const struct wp360_pmuc_sysfs_attribute *data = container_of(attr, struct wp360_pmuc_sysfs_attribute, attribute);
	return sysfs_emit(buf, "%hd\n", data->value);
}

static ssize_t sysfs_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct wp360_pmuc_sysfs_attribute *data = container_of(attr, struct wp360_pmuc_sysfs_attribute, attribute);
	return kstrtou16(buf, 10, &data->value) ? 0 : count;
}

static ssize_t sysfs_ronly(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	return 0;
}

// Dev interface
static dev_t major;

static struct class *cls;

static struct file_operations chardev_fops = {
	.read    = device_read,
	.write   = device_write,
	.open    = device_open,
	.release = device_release,
};

DEFINE_MUTEX(chardev_write);

static char msg[BUF_LEN + 1] = "Hi, this is a weird message\n";

// GPIO and device tree
static struct gpio_desc *gpio_send, *gpio_recv;

// threads and mutex and stuff
static struct task_struct *wp360_pmuc_write_task;

static DECLARE_WAIT_QUEUE_HEAD(waitq);
static atomic_t buffer_op = ATOMIC_INIT(0);
static struct wp360_pmuc_message messages[BUF_LEN];
static struct wp360_pmuc_message_buffer buffer = {0, 0, BUF_LEN, messages};

static int wp360_pmuc_write_thread(void *arg)
{
	pr_info("KThread started\n");
	sched_set_fifo(current);
	pr_info("Scheduler set\n");
	//TODO: proper thread termination, proper signal handling
	while (1)
	{
		pr_info("Looping around...\n");
		if (!atomic_cmpxchg(&buffer_op, 0, 1))
		{
			pr_info("We're in\n");
			// we're in
			while (buffer.push_head != buffer.pop_head)
			{
				pr_info("Popping a message...\n");
				u64 delay  = 0;
				u64 target = 0;
				u64 time   = 0;
				struct wp360_pmuc_message msg = buffer.buffer[buffer.pop_head];
				switch(msg.size)
				{
				case (1):
					pr_info("Sending 0x%02X\n", msg.payload[0]);
					delay = SYNC_BYTE;
					break;
				case (2):
					pr_info("Sending 0x%02X%02X\n", msg.payload[0], msg.payload[1]);
					delay = SYNC_WORD;
					break;
				case (4):
					pr_info("Sending 0x%02X%02X%02X%02X\n", msg.payload[0], msg.payload[1], msg.payload[2], msg.payload[3]);
					delay = SYNC_DWORD;
					break;
				}
				// TODO: gracefully handle wrong size
				gpiod_set_value(gpio_send, 1);
				target = ktime_get_ns() + delay * 1000;
				usleep_range(delay - MSG_DELAY_DELTA, delay - MSG_DELAY_DELTA);
				time = ktime_get_ns();
				if (time < target)
					ndelay(target - time);
				gpiod_set_value(gpio_send, 0);
				target += PAUSE * 1000;
				usleep_range(PAUSE - MSG_DELAY_DELTA, PAUSE - MSG_DELAY_DELTA);
				time = ktime_get_ns();
				if (time < target)
					ndelay(target - time);
				for (int bit = 0; bit < msg.size * 8; bit++)
				{
					gpiod_set_value(gpio_send, 1);
					delay = ((msg.payload[bit >> 3] << (bit & 0x7)) & 0x80) ? PULSE_LENGTH_HIGH : PULSE_LENGTH_LOW;
					target += delay * 1000;
					usleep_range(delay - MSG_DELAY_DELTA, delay - MSG_DELAY_DELTA);
					time = ktime_get_ns();
					if (time < target)
						ndelay(target - time);
					gpiod_set_value(gpio_send, 0);
					target += PAUSE * 1000;
					usleep_range(PAUSE - MSG_DELAY_DELTA, PAUSE - MSG_DELAY_DELTA);
					time = ktime_get_ns();
					if (time < target)
						ndelay(target - time);
				}
				usleep_range(MSG_END_MIN, MSG_END_MAX);
				if (++buffer.pop_head == buffer.buffer_size)
				{
					buffer.pop_head = 0;
				}
			}
			pr_info("We're out\n");
			atomic_set(&buffer_op, 0);
		}
		pr_info("Waking others up\n");
		wake_up(&waitq);
		pr_info("And now going to sleep myself\n");
		wait_event_interruptible(waitq, (pr_info("Test\n"), (!atomic_read(&buffer_op) && (buffer.push_head != buffer.pop_head))));
	}
	return 0;
}

static int devicemodel_probe(struct platform_device *dev)
{
	pr_info("devicemodel probe\n");
	gpio_send = gpiod_get_index(&dev->dev, "comm", 0, GPIOD_OUT_LOW);
	gpio_recv = gpiod_get_index(&dev->dev, "comm", 1, GPIOD_IN);
	if (IS_ERR(gpio_send))
	{
		pr_err("GPIO send request failed: %ld\n", PTR_ERR(gpio_send));
		return PTR_ERR(gpio_send);
	}
	if (IS_ERR(gpio_recv))
	{
		pr_err("GPIO recv request failed: %ld\n", PTR_ERR(gpio_recv));
		return PTR_ERR(gpio_recv);
	}
	return 0;
}

static DEVICEMODEL_REMOVE_RETURN_TYPE devicemodel_remove(struct platform_device *dev)
{
	gpiod_put(gpio_send);
	gpiod_put(gpio_recv);
	DEVICEMODEL_REMOVE_RETURN
}

static int devicemodel_suspend(struct device *dev)
{
	return 0;
}
static int devicemodel_resume (struct device *dev)
{
	return 0;
}

static const struct dev_pm_ops devicemodel_pm_ops = {
	.suspend = devicemodel_suspend,
	.resume = devicemodel_resume,

	.poweroff = devicemodel_suspend,
	.freeze = devicemodel_suspend,

	.thaw = devicemodel_resume,
	.restore = devicemodel_resume,
};

static struct platform_driver devicemodel_driver = {
	.driver = {
		.name = "wp360-pmuc",
		.pm   = &devicemodel_pm_ops,
	},
	.probe = devicemodel_probe,
	.remove = devicemodel_remove,
};


static int __init wp360_pmuc_driver_init(void)
{
	int retval;
	
	major = register_chrdev(0, DEVICE_NAME, &chardev_fops);
	if (major < 0)
	{
		pr_alert("Registering char device failed with %d\n", major);
		return major;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	cls = class_create(DEVICE_NAME);
#else
	cls = class_create(THIS_MODULE, DEVICE_NAME);
#endif
	device_create(cls, NULL, MKDEV(major, 0), NULL, DEVICE_NAME);

	pr_info("Device created on /dev/%s\n", DEVICE_NAME);

	for (int i = 0; i < N_ATTRIBUTES; i++)
	{
		wp360_pmuc_attrs[i] = &attributes[i].attribute.attr;
	}

	retval = kobject_init_and_add(&mymodule, &wp360_pmuc_ktype, kernel_kobj, "%s", "wp360-pmuc");
	if (retval)
		return -ENOMEM;

	retval = platform_driver_register(&devicemodel_driver);
	if (retval)
	{
		pr_err("Unable to register driver\n");
		return retval;
	}

	wp360_pmuc_write_task = kthread_create(wp360_pmuc_write_thread, NULL, "KThread wp360 pmuc send");
	if (IS_ERR(wp360_pmuc_write_task))
	{
		pr_err("Could not create thread\n");
		return -1;
	}
	pr_info("Woken up? %d\n", wake_up_process(wp360_pmuc_write_task));

	pr_info("Driver loaded\n");
	return 0;
}

static void __exit wp360_pmuc_driver_exit(void)
{
	device_destroy(cls, MKDEV(major, 0));
	class_destroy(cls);
	unregister_chrdev(major, DEVICE_NAME);
	kobject_put(&mymodule);
	platform_driver_unregister(&devicemodel_driver);
	pr_info("Driver unloaded\n");
}

static int device_open(struct inode *inode, struct file *file)
{
	// Device was opened, I might want to keep track of this?
	// Definitely gonna have to keep track of this

	return 0;
}

static int device_release(struct inode *inode, struct file *file)

{
	return 0;
}

static ssize_t device_read(struct file *filp, char __user *buffer, size_t length, loff_t *offset)
{
	int bytes_read = 0;
	const char *msg_ptr = msg;

	if (!*(msg_ptr + *offset))
	{
		*offset = 0;
		return 0;
	}

	msg_ptr += *offset;

	while (length && *msg_ptr) {
		/* The buffer is in the user data segment, not the kernel
		 * segment so "*" assignment won't work.  We have to use
		 * put_user which copies data from the kernel data segment to
		 * the user data segment.
		 */
		put_user(*(msg_ptr++), buffer++);
		length--;
		bytes_read++;
	}

	*offset += bytes_read;

	return bytes_read;
}

static ssize_t device_write(struct file *filp, const char __user *buff, size_t len, loff_t *off)
{
	if (*off)
	{
		pr_info("Invalid offset - this should not be possible\n");
		return -EINVAL; // TODO: proper error value invalid seek I think?
	}
	
	switch(len)
	{
		case (1):
		case (2):
		case (4):
			break;
		default:
			pr_info("Invalid number of bytes: %lu\n", len);
			return -EINVAL;
			break;
	}

	if (!access_ok(buff, len))
	{
		pr_info("Couldn't move buffer in kernelspace\n");
		return -EINVAL;
	}

	pr_info("Trying to add to buffer...\n");
	if (atomic_cmpxchg(&buffer_op, 0, 1))
	{
		pr_info("Something's going on, I'll wait a bit\n");
		wait_event_interruptible(waitq, !atomic_cmpxchg(&buffer_op, 0, 1));
	}
	
	if (__copy_from_user(buffer.buffer[buffer.push_head].payload, buff, len))
	{
		pr_err("Could not copy memory from user!\n");
		atomic_set(&buffer_op, 0);
		return -EINVAL;
	}
	buffer.buffer[buffer.push_head].size = len;

	if (++buffer.push_head == buffer.buffer_size)
	{
		pr_info("Buffer wraparound\n");
		buffer.push_head = 0;
	}

	atomic_set(&buffer_op, 0);

	wake_up(&waitq);

	// Read bytes, add them to send buffer
	// Return actual number of written bytes
	return len;
}

module_init(wp360_pmuc_driver_init);
module_exit(wp360_pmuc_driver_exit);
