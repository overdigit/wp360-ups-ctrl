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
#include <linux/interrupt.h>

#include "wp360_pmuc.h"

// Send/receive buffers
static struct wp360_pmuc_message _w_messages[BUF_LEN];
static struct wp360_pmuc_message_buffer write_buffer = {
	.push_head  = 0,
	.pop_head   = 0,
	.size       = BUF_LEN,
	.buffer     = _w_messages,
	.write_lock = ATOMIC_INIT(0)
};

static struct wp360_pmuc_message _r_messages[BUF_LEN];
static struct wp360_pmuc_message_buffer read_buffer  = {
	.push_head  = 0,
	.pop_head   = 0,
	.size       = BUF_LEN,
	.buffer     = _r_messages,
	.write_lock = ATOMIC_INIT(0)
};
static struct wp360_pmuc_message_recv recv_msg;


// Sysfs interface
static struct kobject mymodule;

#define N_ATTRIBUTES (sizeof(attributes)/sizeof(struct wp360_pmuc_sysfs_attribute))
static struct wp360_pmuc_sysfs_attribute attributes[] = {
	{MSG_SYS_POWEROFF,          0,      1,  0, __ATTR(sys_poweroff,          0200, sysfs_wonly, sysfs_storf) },
	{MSG_BOOTUP,                0,      1,  0, __ATTR(bootup,                0200, sysfs_wonly, sysfs_storf) },
	{MSG_SHUTDOWN_GUARD,        0,      1,  0, __ATTR(shutdown_guard,        0644, sysfs_show,  sysfs_storf) },
	{MSG_POWER_STATE,           0,      0,  0, __ATTR(power_state,           0444, sysfs_show,  sysfs_ronly) },

	{MSG_POWER_VOLTAGE_NOMINAL, 105,  300,  0, __ATTR(power_voltage_nominal, 0644, sysfs_show,  sysfs_storb) },
	{MSG_POWER_VOLTAGE_MIN,     90,   300,  0, __ATTR(power_voltage_min,     0644, sysfs_show,  sysfs_storb) },
	{MSG_CAPACITOR_VOLTAGE_MIN, 52,   140,  0, __ATTR(capacitor_voltage_min, 0644, sysfs_show,  sysfs_storb) },
	{MSG_SWITCHING_VOLTAGE_MIN, 50,   300,  0, __ATTR(switching_voltage_min, 0644, sysfs_show,  sysfs_storb) },
	{MSG_BATTERY_VOLTAGE_MIN,   50,   140,  0, __ATTR(battery_voltage_min,   0644, sysfs_show,  sysfs_storb) },
	{MSG_PROGRAM_VERSION,       0,    255,  0, __ATTR(program_version,       0644, sysfs_show,  sysfs_storb) },
	{MSG_PORT_POWEROFF,         0,    255,  0, __ATTR(port_poweroff,         0644, sysfs_show,  sysfs_storb) },
	{MSG_SWITCHING_TIMEOUT,     0, 0xFFFE,  0, __ATTR(switching_timeout,     0644, sysfs_show,  sysfs_storb) },

	{MSG_POWER_VOLTAGE,         0,      0,  1, __ATTR(power_voltage,         0444, sysfs_query, sysfs_ronly) },
	{MSG_CAPACITOR_VOLTAGE,     0,      0,  1, __ATTR(capacitor_voltage,     0444, sysfs_query, sysfs_ronly) },
	{MSG_SWITCHING_VOLTAGE,     0,      0,  1, __ATTR(switching_voltage,     0444, sysfs_query, sysfs_ronly) },
	{MSG_PMUC_TEMPERATURE,      0,      0,  1, __ATTR(pmuc_temperature,      0444, sysfs_query, sysfs_ronly) },
	{MSG_FAN_VOLTAGE,           20,    90,  0, __ATTR(fan_voltage,           0644, sysfs_show,  sysfs_storb) },
	{MSG_WATCHDOG_ENABLE,       0, 0xFFFE,  0, __ATTR(watchdog_enable,       0644, sysfs_show,  sysfs_storw) },
	{MSG_WATCHDOG_TRIGGER,      0,      0,  1, __ATTR(watchdog_trigger,      0444, sysfs_query, sysfs_ronly), .querying = ATOMIC_INIT(1)},
};

static struct attribute *wp360_pmuc_attrs[N_ATTRIBUTES + 1];
ATTRIBUTE_GROUPS(wp360_pmuc);

static const struct kobj_type wp360_pmuc_ktype = {
	.sysfs_ops = &kobj_sysfs_ops,
	.default_groups = wp360_pmuc_groups,
};

static ssize_t sysfs_wonly(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return -EINVAL;
}

static ssize_t sysfs_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	const struct wp360_pmuc_sysfs_attribute *data = container_of(attr, struct wp360_pmuc_sysfs_attribute, attribute);
	return sysfs_emit(buf, "%hd\n", data->value);
}

static ssize_t sysfs_query(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct wp360_pmuc_sysfs_attribute *data = container_of(attr, struct wp360_pmuc_sysfs_attribute, attribute);
	int ret = 0;
	u8 querying = 0;
	u8 cmd = data->cmd;

	if (!atomic_cmpxchg(&data->querying, 0, 1))
	{
		querying = 1;
		if (atomic_cmpxchg(&write_buffer.write_lock, 0, 1))
		{ // Another write is in progress
			ret = wait_event_interruptible(write_buffer.waitq, !atomic_cmpxchg(&write_buffer.write_lock, 0, 1));
			if (ret)
			{
				atomic_set(&data->querying, 0);
				return -EINTR;
			}
		}	
		data->value = 0xFFFF;
		write_buffer.buffer[write_buffer.push_head].payload[0] = cmd;
		write_buffer.buffer[write_buffer.push_head].size = 1;

		if (++write_buffer.push_head == write_buffer.size)
		{
			write_buffer.push_head = 0;
		}

		atomic_set(&write_buffer.write_lock, 0);
		wake_up(&write_buffer.waitq);
	}

	pr_info("[sysfs] Waiting for 0x%02X\n", data->cmd);
	ret = wait_event_interruptible(data->waitq, (pr_info("[sysfs] Waking up?\n"), data->value != 0xFFFF));
	pr_info("[sysfs] Woke up\n");
	if (querying)
	{
		atomic_set(&data->querying, 0);
	}
	if (ret)
	{
		pr_info("[sysfs] Interrupted\n");
		return -EINTR;
	}
	return sysfs_emit(buf, "%hd\n", data->value);
}


static ssize_t sysfs_storf(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	return sysfs_store(kobj, attr, buf, count, 1);
}
static ssize_t sysfs_storb(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	return sysfs_store(kobj, attr, buf, count, 2);
}
static ssize_t sysfs_storw(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	return sysfs_store(kobj, attr, buf, count, 3);
}
static ssize_t sysfs_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count, size_t size)
{
	u16     value;
	struct  wp360_pmuc_sysfs_attribute *data = container_of(attr, struct wp360_pmuc_sysfs_attribute, attribute);
	struct  wp360_pmuc_message *msg;
	u8 cmd = data->cmd;;

	if (kstrtou16(buf, 10, &value))
	{
		return -EINVAL;
	}
	if (value < data->min_value || value > data->max_value)
	{
		return -EINVAL;
	}
	if (atomic_cmpxchg(&write_buffer.write_lock, 0, 1))
	{ // Another write is in progress
		if (wait_event_interruptible(write_buffer.waitq, !atomic_cmpxchg(&write_buffer.write_lock, 0, 1)))
		{
			return -EINTR;
		}
	}
	msg = write_buffer.buffer + write_buffer.push_head;
	switch(size)
	{
	case (1):
		msg->payload[0]  = cmd;
		msg->payload[0] |= MSG_WRITE_MASK;
		msg->payload[0] |= value ? 1 : 0;
		break;
	case (2):
		msg->payload[0]  = cmd;
		msg->payload[0] |= MSG_WRITE_MASK;
		msg->payload[0] |= ( value & 0x0100) ? 1 : 0;
		msg->payload[1]  = value & 0xFF;
		break;
	case (3):
		msg->payload[0]  = cmd;
		msg->payload[0] |= MSG_WRITE_MASK;
		msg->payload[1]  = value >> 8;
		msg->payload[2]  = value & 0xFF;
		break;
	default:
		return -EINVAL;
	}
	msg->size = size;

	if (++write_buffer.push_head == write_buffer.size)
	{
		write_buffer.push_head = 0;
	}

	atomic_set(&write_buffer.write_lock, 0);
	wake_up(&write_buffer.waitq);

	return count;
}

static ssize_t sysfs_ronly(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	return -EINVAL;
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

// GPIO and device tree
static struct gpio_desc *gpio_send, *gpio_recv;

// threads and mutex and stuff
static struct task_struct *wp360_pmuc_write_task;

static int irq;

static int wp360_pmuc_write_thread(void *arg)
{
	sched_set_fifo(current);
	int ret;
	while (1)
	{
		if (!atomic_cmpxchg(&write_buffer.write_lock, 0, 1))
		{
			while (write_buffer.push_head != write_buffer.pop_head)
			{
				u64 delay  = 0;
				u64 target = 0;
				u64 time   = 0;
				struct wp360_pmuc_message msg = write_buffer.buffer[write_buffer.pop_head];
				switch(msg.size)
				{
				case (1):
					pr_info("[w] Sending 0x%02X\n", msg.payload[0]);
					delay = SYNC_BYTE;
					break;
				case (2):
					pr_info("[w] Sending 0x%02X%02X\n", msg.payload[0], msg.payload[1]);
					delay = SYNC_WORD;
					break;
				case (3):
					pr_info("[w] Sending 0x%02X%02X%02X\n", msg.payload[0], msg.payload[1], msg.payload[2]);
					delay = SYNC_HWORD;
					break;
				case (4):
					pr_info("[w] Sending 0x%02X%02X%02X%02X\n", msg.payload[0], msg.payload[1], msg.payload[2], msg.payload[3]);
					delay = SYNC_DWORD;
					break;
				case (5):
					pr_info("[w] Sending 0x%02X%02X%02X%02X%02X\n", msg.payload[0], msg.payload[1], msg.payload[2], msg.payload[3], msg.payload[4]);
					delay = SYNC_PWORD;
					break;
				default:
					pr_info("[w] Wrong message size in buffer, ignoring\n");
					break;
				}
				if (delay)
				{
					gpiod_set_value(gpio_send, 1);
					target = ktime_get_ns();
					PRECISE_SLEEP(delay);
					gpiod_set_value(gpio_send, 0);
					PRECISE_SLEEP(PAUSE);				
					for (int bit = 0; bit < msg.size * 8; bit++)
					{
						gpiod_set_value(gpio_send, 1);
						delay = ((msg.payload[bit >> 3] << (bit & 0x7)) & 0x80) ? PULSE_LENGTH_HIGH : PULSE_LENGTH_LOW;
						PRECISE_SLEEP(delay);
						gpiod_set_value(gpio_send, 0);
						PRECISE_SLEEP(PAUSE);
					}
				}
				usleep_range(MSG_END_MIN, MSG_END_MAX);
				if (++write_buffer.pop_head == write_buffer.size)
				{
					write_buffer.pop_head = 0;
				}
			}
			atomic_set(&write_buffer.write_lock, 0);
		}
		wake_up(&write_buffer.waitq);
		ret = wait_event_interruptible(write_buffer.waitq, !atomic_read(&write_buffer.write_lock) && (write_buffer.push_head != write_buffer.pop_head));
		if (ret == -ERESTARTSYS)
		{ // Received signal, terminate
			pr_info("Received signal, exiting\n");
			return -EINTR;
		}
	}
	return 0;
}

static irqreturn_t wp360_pmuc_interrupt(int irq, void *dev_id)
{
	u64 time = ktime_get_ns();
	int state = gpiod_get_value(gpio_recv);
	switch (state)
	{
	case (1): // Line is active low
		recv_msg.fall_time = time;
		break;
	case (0):
		u64 dt = time - recv_msg.fall_time;
		if (dt >= 300000 && dt <= 700000)
		{ // Low bit
			recv_msg.msg.payload[recv_msg.bit >> 3] <<= 1;
		}
		else if (dt >= 800000 && dt <= 1200000)
		{ // High bit
			recv_msg.msg.payload[recv_msg.bit >> 3] <<= 1;
			recv_msg.msg.payload[recv_msg.bit >> 3] |=  1;
		}
		else if (dt >= 1700000 && dt <= 2300000)
		{ // Sync, byte incoming
			recv_msg.msg.size = 1;
			recv_msg.bit      = -1;
		}
		else if (dt >= 2700000 && dt <= 3300000)
		{ // Sync, word incoming
			recv_msg.msg.size = 2;
			recv_msg.bit      = -1;
		}
		else if (dt >= 3700000 && dt <= 4300000)
		{ // Sync, word+ incoming
			recv_msg.msg.size = 3;
			recv_msg.bit      = -1;
		}
		else if (dt >= 4700000 && dt <= 5300000)
		{ // Sync, dword incoming
			recv_msg.msg.size = 4;
			recv_msg.bit      = -1;
		}
		else if (dt >= 5700000 && dt <= 6300000)
		{ // Sync, dword+ incoming
			recv_msg.msg.size = 5;
			recv_msg.bit      = -1;
		}
		else
		{ // Invalid signal, scrap everything
			recv_msg.msg.size = 0;
			recv_msg.bit      = 127;
		}
		if (recv_msg.bit != 127)
		{
			recv_msg.bit++;
			if (recv_msg.bit == (recv_msg.msg.size << 3))
			{
				read_buffer.buffer[read_buffer.push_head++] = recv_msg.msg;
				read_buffer.push_head &= read_buffer.size - 1;
				return IRQ_WAKE_THREAD;
			}
			else if (recv_msg.bit > (recv_msg.msg.size << 3))
			{
				recv_msg.bit = 127;
			}
		}
		break;
	default:
		pr_err("Invalid gpio_recv state %d\n", state);
		return IRQ_HANDLED;
		break;
	}
	return IRQ_HANDLED;
}

static irqreturn_t wp360_pmuc_interrupt_thread(int irq, void *dev_id)
{
	int push_head = read_buffer.push_head;
	wake_up(&read_buffer.waitq);
	while (push_head != read_buffer.pop_head)
	{
		struct wp360_pmuc_message *msg = read_buffer.buffer + read_buffer.pop_head++;
		u8  cmd = msg->payload[0] & MSG_CMD_MASK;
		u64 value;
		switch(msg->size)
		{
		case (1):
			value = msg->payload[0] & MSG_FLAG_MASK;
			break;
		case (2):
			pr_info("0x%02X%02X\n", msg->payload[0], msg->payload[1]);
			value = READ_MSG_WORD(msg);
			break;
		case (3):
			pr_info("0x%02X%02X%02X\n", msg->payload[0], msg->payload[1], msg->payload[2]);
			value = (msg->payload[1] << 8) | msg->payload[2];
			break;
		case (4):
			value = (msg->payload[1] << 16) | (msg->payload[2] << 8) | msg->payload[3];
			break;
		case (5):
			value = (msg->payload[1] << 24) | (msg->payload[2] << 16) | (msg->payload[3] << 8) | msg->payload[4];
			break;
		}
		pr_info("Received 0x%02X[%d] %llX\n", cmd, msg->size, value);
		switch (cmd)
		{
		case (MSG_SYS_POWEROFF):
			if (value)
			{
				static char * shutdown_argv[] = {"/sbin/shutdown", "-h", "-P", "now", NULL};
				call_usermodehelper(shutdown_argv[0], shutdown_argv, NULL, UMH_NO_WAIT);
			}
			break;
		case (MSG_POWER_STATE):
		case (MSG_POWER_VOLTAGE):
		case (MSG_CAPACITOR_VOLTAGE):
		case (MSG_SWITCHING_VOLTAGE):
		case (MSG_POWER_VOLTAGE_NOMINAL):
		case (MSG_POWER_VOLTAGE_MIN):
		case (MSG_CAPACITOR_VOLTAGE_MIN):
		case (MSG_SWITCHING_VOLTAGE_MIN):
		case (MSG_BATTERY_VOLTAGE_MIN):
		case (MSG_PROGRAM_VERSION):
		case (MSG_PORT_POWEROFF):
		case (MSG_SWITCHING_TIMEOUT):
		case (MSG_PMUC_TEMPERATURE):
		case (MSG_FAN_VOLTAGE):
		case (MSG_WATCHDOG_ENABLE):
		case (MSG_WATCHDOG_TRIGGER):
			struct wp360_pmuc_sysfs_attribute *data;
			for (int i = 0; i < sizeof(attributes) / sizeof(struct wp360_pmuc_sysfs_attribute); i++)
			{
				if (attributes[i].cmd == cmd)
				{
					data = attributes + i;
					break;
				}
			}
			data->value = value;
			if (data->has_queue)
			{
				wake_up(&data->waitq);
			}
			break;
		}
		read_buffer.pop_head &= read_buffer.size - 1;
	}
	return IRQ_HANDLED;
}

static int devicemodel_probe(struct platform_device *dev)
{
	pr_info("devicemodel probe\n");
	gpio_send = gpiod_get_index(&dev->dev, "comm", 0, GPIOD_OUT_LOW);
	if (IS_ERR(gpio_send))
	{
		pr_err("GPIO send request failed: %ld\n", PTR_ERR(gpio_send));
		return PTR_ERR(gpio_send);
	}

	gpio_recv = gpiod_get_index(&dev->dev, "comm", 1, GPIOD_IN);
	if (IS_ERR(gpio_recv))
	{
		gpiod_put(gpio_send);
		pr_err("GPIO recv request failed: %ld\n", PTR_ERR(gpio_recv));
		return PTR_ERR(gpio_recv);
	}

	irq = gpiod_to_irq(gpio_recv);
	if (irq < 0)
	{
		pr_err("GPIO to IRQ failed\n");
		return irq;
	}

	int ret = request_threaded_irq(irq, wp360_pmuc_interrupt, wp360_pmuc_interrupt_thread, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING, DEVICE_NAME, (void *) dev);
	if (ret)
	{
		pr_err("Could not request IRQ\n");
		gpiod_put(gpio_recv);
		gpiod_put(gpio_send);
		return ret;
	}
	
	return 0;
}

static DEVICEMODEL_REMOVE_RETURN_TYPE devicemodel_remove(struct platform_device *dev)
{
	gpiod_put(gpio_send);
	gpiod_put(gpio_recv);
	free_irq(irq, (void *) dev);
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
	// TODO: move most code from here to device_probe
	// TODO: put most static vars to a struct, allocate it in device_probe, free it on release
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
	cls->dev_groups = wp360_pmuc_groups;
	device_create(cls, NULL, MKDEV(major, 0), NULL, DEVICE_NAME);

	pr_info("Device created on /dev/%s\n", DEVICE_NAME);
	
	init_waitqueue_head(&write_buffer.waitq);
	init_waitqueue_head(&read_buffer.waitq);

	for (int i = 0; i < N_ATTRIBUTES; i++)
	{
		if (attributes[i].has_queue)
		{
			init_waitqueue_head(&attributes[i].waitq);
		}
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

	if (file->f_mode & FMODE_READ)
	{
		struct wp360_pmuc_device_read_head *p = kzalloc(sizeof(struct wp360_pmuc_device_read_head), GFP_KERNEL);
		if (!p)
		{
			pr_err("Could not allocate memory for device open\n");
			return -ENOMEM;
		}
		file->private_data = p;
		p->waitq = &read_buffer.waitq;
		p->pop_head = read_buffer.pop_head;
	}
	else
	{
		file->private_data = NULL;
	}
	return 0;
}

static int device_release(struct inode *inode, struct file *file)
{
	if (file->private_data != NULL)
	{
		kfree(file->private_data);
	}
	return 0;
}

static ssize_t device_read(struct file *filp, char __user *buffer, size_t length, loff_t *offset)
{
	// TODO: if non-blocking, return -EAGAIN instead of waiting
	struct wp360_pmuc_device_read_head *rh    = filp->private_data;
	struct wp360_pmuc_message           *msg;
	pr_info("[read] offset = %p\n", offset);

	if (rh->pop_head == read_buffer.push_head)
	{
		if (wait_event_interruptible(*rh->waitq, (pr_info("[read] Woken up?\n"), rh->pop_head != read_buffer.push_head)))
		{
			return -EINTR;
		}
	}

	pr_info("[read] Woken up\n");
	msg = read_buffer.buffer + rh->pop_head;
	if (copy_to_user(buffer, msg->payload, msg->size))
	{
		// TODO: proper return value
		return -EINVAL;
	}

	rh->pop_head = (rh->pop_head + 1) & (read_buffer.size - 1);
	pr_info("[read] Returned %u bytes\n", msg->size);

	return msg->size;
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
		case (3):
		case (4):
		case (5):
			break;
		default:
			pr_info("Invalid number of bytes: %lu\n", len);
			return -EINVAL;
			break;
	}

	if (!access_ok(buff, len))
	{
		pr_info("Couldn't access user buffer\n");
		return -EINVAL;
	}

	if (atomic_cmpxchg(&write_buffer.write_lock, 0, 1))
	{ // Another write is in progress
	  // TODO: return -EAGAIN if blocking
		if (wait_event_interruptible(write_buffer.waitq, !atomic_cmpxchg(&write_buffer.write_lock, 0, 1)))
		{
			return -EINTR;
		}
	}
	
	if (__copy_from_user(write_buffer.buffer[write_buffer.push_head].payload, buff, len))
	{
		pr_err("Could not copy memory from user!\n");
		atomic_set(&write_buffer.write_lock, 0);
		return -EINVAL;
	}
	write_buffer.buffer[write_buffer.push_head].size = len;

	if (++write_buffer.push_head == write_buffer.size)
	{
		pr_info("Buffer wraparound\n");
		write_buffer.push_head = 0;
	}

	atomic_set(&write_buffer.write_lock, 0);

	wake_up(&write_buffer.waitq);

	return len;
}

module_init(wp360_pmuc_driver_init);
module_exit(wp360_pmuc_driver_exit);
