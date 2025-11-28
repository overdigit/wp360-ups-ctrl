#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
#define DEVICEMODEL_REMOVE_RETURN_TYPE void
#define DEVICEMODEL_REMOVE_RETURN
#else
#define DEVICEMODEL_REMOVE_RETURN_TYPE int
#define DEVICEMODEL_REMOVE_RETURN      return 0;
#endif

#define DEVICE_NAME "wp360-pmuc"
#define BUF_LEN     32

#define SYNC_BYTE         2000
#define SYNC_WORD         3000
#define SYNC_HWORD        4000
#define SYNC_DWORD        5000
#define SYNC_PWORD        6000
#define PAUSE             500
#define PULSE_LENGTH_HIGH 1000
#define PULSE_LENGTH_LOW  500
#define MSG_END_MIN       1500
#define MSG_END_MAX       2000
#define MSG_DELAY_DELTA   5

#define MSG_MAX_SIZE      5

// Module metadata
MODULE_AUTHOR("Nicola Orlando");
MODULE_DESCRIPTION("WP360 power management microcontroller driver");
MODULE_LICENSE("GPL");

static int device_open   (struct inode *, struct file *);
static int device_release(struct inode *, struct file *);

static ssize_t device_read (struct file *,       char __user *, size_t, loff_t *);
static ssize_t device_write(struct file *, const char __user *, size_t, loff_t *);

struct wp360_pmuc_device_read_head {
	wait_queue_head_t *waitq;
	size_t             pop_head;
};

static ssize_t sysfs_show (struct kobject *, struct kobj_attribute *,       char *);
static ssize_t sysfs_query(struct kobject *, struct kobj_attribute *,       char *);
static ssize_t sysfs_wonly(struct kobject *, struct kobj_attribute *,       char *);
static ssize_t sysfs_storf(struct kobject *, struct kobj_attribute *, const char *, size_t);
static ssize_t sysfs_storw(struct kobject *, struct kobj_attribute *, const char *, size_t);
static ssize_t sysfs_store(struct kobject *, struct kobj_attribute *, const char *, size_t, size_t);
static ssize_t sysfs_ronly(struct kobject *, struct kobj_attribute *, const char *, size_t);


struct wp360_pmuc_sysfs_attribute {
	const  u8             cmd;
	const  u16            min_value;
	const  u16            max_value;
	const  u8             has_queue;
	struct kobj_attribute attribute;
	u16                   value;
	wait_queue_head_t     waitq;
	atomic_t              querying;
};

static int  devicemodel_probe  (struct platform_device *);
static DEVICEMODEL_REMOVE_RETURN_TYPE devicemodel_remove (struct platform_device *);
static int  devicemodel_suspend(struct device *);
static int  devicemodel_resume (struct device *);

#define USLEEP(usec)        usleep_range(usec - MSG_DELAY_DELTA, usec - MSG_DELAY_DELTA)
#define PRECISE_SLEEP(usec) { target += usec*1000; USLEEP(usec); time = ktime_get_ns(); if (time < target) ndelay(target-time); }
static int  wp360_pmuc_write_thread(void *);

static irqreturn_t wp360_pmuc_interrupt       (int, void *);
static irqreturn_t wp360_pmuc_interrupt_thread(int, void *);

struct wp360_pmuc_message {
	u8 size;
	u8 payload[MSG_MAX_SIZE];
};

struct wp360_pmuc_message_recv {
	struct wp360_pmuc_message msg;
	u8  bit;
	u64 fall_time;
};

struct wp360_pmuc_message_buffer {
	size_t                     push_head;
	size_t                     pop_head;
	size_t                     size;
	struct wp360_pmuc_message *buffer;
	atomic_t                   write_lock;
	wait_queue_head_t          waitq;
};

/*
struct wp360_pmuc_device {
	struct wp360_pmuc_message_buffer write_buffer;
	struct wp360_pmuc_message_buffer read_buffer;
	struct wp360_pmuc_message_recv   recv_msg;

	struct gpio_desc                *gpio_send;
	struct gpio_desc                *gpio_recv;
	struct task_struct              *write_task;
	int                              irq;
}
//*/

// Codifica dei messaggi CPU <-> UPS.
#define MSG_WRITE_MASK             0x80   // bit messaggio in scrittura
#define MSG_CMD_MASK               0x3E   // maschera selezione comando
#define MSG_FLAG_MASK              0x01   // maschera selezione dato flag
#define MSG_WORD_MASK              0x01FF // maschera selezione dato word (max 9 bits)
//
#define MSG_SYS_POWEROFF           0x02   // shutdown CPU concluso
#define MSG_BOOTUP                 0x04   // CPU avviata, utilizzare shutdown regolare
#define MSG_SHUTDOWN_GUARD         0x06   // richiesta attesa per eventuale shutdown
#define MSG_POWER_STATE            0x08   // stato presenza rete
//
#define MSG_POWER_VOLTAGE_NOMINAL  0x10   // tensione di alimentazione nominale del dispositivo
#define MSG_POWER_VOLTAGE_MIN      0x12   // tensione di alimentazione minima per mancanza rete
#define MSG_CAPACITOR_VOLTAGE_MIN  0x14   // tensione di fine carica supercap per avvio
#define MSG_SWITCHING_VOLTAGE_MIN  0x16   // tensione minima ingresso regolatore switching
#define MSG_BATTERY_VOLTAGE_MIN    0x18   // tensione minima batteria tampone
#define MSG_PROGRAM_VERSION        0x1A   // versione a batteria tampone
#define MSG_PORT_POWEROFF          0x1C   // configurazione spegnimento porte
#define MSG_SWITCHING_TIMEOUT      0x1E   // timeout spegnimento switching forzato
//
#define MSG_POWER_VOLTAGE          0x20   // tensione di alimentazione attuale
#define MSG_CAPACITOR_VOLTAGE      0x22   // tensione di carica supercapacitor attuale
#define MSG_SWITCHING_VOLTAGE      0x24   // tensione su ingresso switching attuale
#define MSG_PMUC_TEMPERATURE       0x26   // temperatura pmuc
#define MSG_FAN_VOLTAGE            0x28   // tensione ventole (20-90% -> 2.65-5V)
#define MSG_WATCHDOG_ENABLE        0x2C   // abilitazione sul PMUC del watchdog CPU
#define MSG_WATCHDOG_TRIGGER       0x2E   // scadenza del watchdog CPU

#define READ_MSG_WORD(msg) (((msg->payload[0] << 8) + msg->payload[1]) & MSG_WORD_MASK)
