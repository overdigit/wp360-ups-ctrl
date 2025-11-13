#define DEVICE_NAME "wp360-pmuc"
#define BUF_LEN     4

// Module metadata
MODULE_AUTHOR("Nicola Orlando");
MODULE_DESCRIPTION("WP360 power management microcontroller driver");
MODULE_LICENSE("GPL");

enum
{
	CDEV_NOT_USED,
	CDEV_EXCLUSIVE_OPEN,
};

static int device_open   (struct inode *, struct file *);
static int device_release(struct inode *, struct file *);

static ssize_t device_read (struct file *,       char __user *, size_t, loff_t *);
static ssize_t device_write(struct file *, const char __user *, size_t, loff_t *);

static ssize_t sysfs_show (struct kobject *, struct kobj_attribute *,       char *);
static ssize_t sysfs_store(struct kobject *, struct kobj_attribute *, const char *, size_t);
static ssize_t sysfs_ronly(struct kobject *, struct kobj_attribute *, const char *, size_t);

struct wp360_pmuc_sysfs_attribute {
	const  char *const    name;
	const  u16            min_value;
	const  u16            max_value;
	struct kobj_attribute attribute;
	u16                   value;
};
