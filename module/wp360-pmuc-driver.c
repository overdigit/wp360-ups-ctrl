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
#include <asm/errno.h>

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
/*static struct attribute_group attribute_grp = {
	.attrs = sysfs_attributes,
};*/

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

static atomic_t already_open = ATOMIC_INIT(CDEV_NOT_USED);

static char msg[BUF_LEN + 1];

// Kernel module parameters
static int gpio_read_pin  = 23;
static int gpio_write_pin = 24;

module_param(gpio_read_pin, int, 0000);
MODULE_PARM_DESC(gpio_read_pin, "The pin the PMUC send line is wired to");

module_param(gpio_write_pin, int, 0000);
MODULE_PARM_DESC(gpio_write_pin, "The pin the PMUC receive line is wired to");


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

	return 0;
}

static void __exit wp360_pmuc_driver_exit(void)
{
	device_destroy(cls, MKDEV(major, 0));
	class_destroy(cls);
	unregister_chrdev(major, DEVICE_NAME);
	kobject_put(&mymodule);
}

static int device_open(struct inode *inode, struct file *file)
{
  // Device was opened, I might want to keep track of this?
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
	// Read bytes, add them to send buffer
	// Return actual number of written bytes
	return -EINVAL;
}

module_init(wp360_pmuc_driver_init);
module_exit(wp360_pmuc_driver_exit);
