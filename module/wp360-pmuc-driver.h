#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
#define DEVICEMODEL_REMOVE_RETURN_TYPE void
#define DEVICEMODEL_REMOVE_RETURN
#else
#define DEVICEMODEL_REMOVE_RETURN_TYPE int;
#define DEVICEMODEL_REMOVE_RETURN      return 0;
#endif

#define DEVICE_NAME "wp360-pmuc"
#define BUF_LEN     32

#define SYNC_BYTE         2000
#define SYNC_WORD         3000
#define SYNC_DWORD        4000
#define PAUSE             500
#define PULSE_LENGTH_HIGH 1000
#define PULSE_LENGTH_LOW  500
#define MSG_END_MIN       1500
#define MSG_END_MAX       2000
#define MSG_DELAY_DELTA   5

// Module metadata
MODULE_AUTHOR("Nicola Orlando");
MODULE_DESCRIPTION("WP360 power management microcontroller driver");
MODULE_LICENSE("GPL");

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

static int  devicemodel_probe  (struct platform_device *);
static DEVICEMODEL_REMOVE_RETURN_TYPE
            devicemodel_remove (struct platform_device *);
static int  devicemodel_suspend(struct device *);
static int  devicemodel_resume (struct device *);

static int  wp360_pmuc_write_thread(void *arg);

struct wp360_pmuc_message {
	char size;
	char payload[4];
};

struct wp360_pmuc_message_buffer {
	size_t push_head;
	size_t pop_head;
	size_t buffer_size;
	struct wp360_pmuc_message *buffer;
};
