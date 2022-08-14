/**
    Drivers for usb device plug and play detection
    @file usb_driver.c
    @author Wang Taorui
    @version 2022/8/14
*/
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/fb.h>
#include <linux/notifier.h>
#include <linux/proc_fs.h>
#include <linux/syscore_ops.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/ktime.h>
#include <linux/time.h>
#include <linux/mutex.h>
#include <linux/version.h>


#define LOGI(...)	(pr_info(__VA_ARGS__))
#define LOGE(...)	(pr_err(__VA_ARGS__))


#define MESSAGE_BUFFER_SIZE	512
#define CMD_GET_STATUS	_IOR(0xFF, 123, unsigned char)


#define OUT
#define IN


struct usb_message_t {
    signed long long kernel_time;   // 8 bytes;
    char             plug_flag;     // 1 means insert usb,0 means unplug usb;
    char             usb_name[32];
};


struct usb_monitor_t {
    struct notifier_block fb_notif;
    struct usb_message_t message[MESSAGE_BUFFER_SIZE];
    int    usb_message_count;      // Number of recorded data;
    int    usb_message_index_read; // Read adress;
    int    usb_message_index_write;// Write adress;
    int    enable_usb_monitor;
    char   write_buff[10];
    char*  init_flag;

    wait_queue_head_t usb_monitor_queue;           // Define wait queue head;
    struct            mutex usb_monitor_mutex;     // Mutex;
};


static struct usb_monitor_t *monitor;
static char *TAG = "MONITOR";


/**
 * Implementation of the read interface
 * 
 * @param filp;
 * @param buf;
 * @param ppos;
 *
 * @return size of message;
 */
static ssize_t usb_monitor_read(struct file *filp, char __user *buf, size_t size, loff_t *ppos){
    int index;
    size_t message_size = sizeof(struct usb_message_t);

    LOGI("%s:%s\n", TAG, __func__);

    if (size < message_size) {
        LOGE("%s:read size is smaller than message size!\n", TAG);
        return -EINVAL;
    }

    wait_event_interruptible(monitor->usb_monitor_queue, monitor->usb_message_count > 0);
    LOGI("%s:read wait event pass\n", TAG);

    // Get lock;
    mutex_lock(&monitor->usb_monitor_mutex);

    // Read data from kernel space to user space if there is data in the circular queue;
    if (monitor->usb_message_count > 0) {
        index = monitor->usb_message_index_read;

        if (copy_to_user(buf, &monitor->message[index], message_size)) {
            LOGE("%s:copy_from_user error!\n", TAG);
            // Unlock;
            mutex_unlock(&monitor->usb_monitor_mutex);
            return -EFAULT;
        }

        // Add 1 to the read address in the circular queue;
        monitor->usb_message_index_read++;

        // If the read address exceeds the maximum value of the circular queue address, 
        // set to zero;
        if (monitor->usb_message_index_read >= MESSAGE_BUFFER_SIZE){
            monitor->usb_message_index_read = 0;
        }

        // After reading the data, the total amount of data in the queue is subtracted by one
        monitor->usb_message_count--;
    }

    // Unlock;
    mutex_unlock(&monitor->usb_monitor_mutex);

    LOGI("%s:read count:%d\n", TAG, message_size);

    return message_size;
}


/**
 * Implementation of the write interface
 * 
 * @param filp;
 * @param buf;
 * @param ppos;
 *
 * @return size of message;
 */
static ssize_t usb_monitor_write(struct file *filp, const char __user *buf, size_t size, loff_t *ppos){
    char end_flag = 0x0a, cmd;

    LOGI("%s:%s\n", TAG, __func__);

    /* only support size=2, such as "echo 0 > usb_monitor" */
    if (size != 2) {
        LOGE("%s:invalid cmd size: size = %d\n", TAG, (int)size);
        return -EINVAL;
    }

    if (copy_from_user(monitor->write_buff, buf, size)) {
        LOGE("%s:copy_from_user error!\n", TAG);
        return -EFAULT;
    }

    if (monitor->write_buff[1] != end_flag) {
        LOGE("%s:invalid cmd: end_flag != 0x0a\n", TAG);
        return -EINVAL;
    }
    cmd = monitor->write_buff[0];

    mutex_lock(&monitor->usb_monitor_mutex);

    switch (cmd) {
    case '0':
        monitor->enable_usb_monitor = 0;
        LOGI("%s:disable usb monitor\n", TAG);
        break;
    case '1':
        monitor->enable_usb_monitor = 1;
        LOGI("%s:enable usb monitor\n", TAG);
        break;
    default:
        LOGE("%s:invalid cmd: cmd = %d\n", TAG, cmd);
        mutex_unlock(&monitor->usb_monitor_mutex);
        return -EINVAL;
  }

  mutex_unlock(&monitor->usb_monitor_mutex);

  return size;
}


/**
 * Implementation of the poll interface
 *
 * @param filp;
 * @param wait;
 *
 * @return mask
 */
static unsigned int usb_monitor_poll(struct file *filp, struct poll_table_struct *wait){
    unsigned int mask = 0;

    LOGI("%s:%s\n", TAG, __func__);

    poll_wait(filp, &monitor->usb_monitor_queue, wait);

    mutex_lock(&monitor->usb_monitor_mutex);
    if (monitor->usb_message_count > 0){
        mask |= POLLIN | POLLRDNORM;
    }
    mutex_unlock(&monitor->usb_monitor_mutex);

    return mask;
}


/**
 * Implementation of the ioctl interface
 *
 * @param filp;
 * @param cmd;
 * @param arg;
 *
 * @return 0;
 */
static long usb_monitor_ioctl(struct file *filp, unsigned int cmd, unsigned long arg){
    void __user *ubuf = (void __user *)arg;
    unsigned char status;

    LOGI("%s:%s\n", TAG, __func__);

    mutex_lock(&monitor->usb_monitor_mutex);

    switch (cmd) {
    case CMD_GET_STATUS:
        LOGI("%s:ioctl:get enable status\n", TAG);
        if (monitor->enable_usb_monitor == 0)
            status = 0x00;
        else
            status = 0xff;

        LOGI("%s:ioctl:status=0x%x\n", TAG, status);

        if (copy_to_user(ubuf, &status, sizeof(status))) {
            LOGE("%s:ioctl:copy_to_user fail\n", TAG);
            mutex_unlock(&monitor->usb_monitor_mutex);
            return -EFAULT;
        }
        break;
    default:
        LOGE("%s:invalid cmd\n", TAG);
        mutex_unlock(&monitor->usb_monitor_mutex);
        return -ENOTTY;
    }

    mutex_unlock(&monitor->usb_monitor_mutex);

    return 0;
}


// Register a node in /proc according to the version of the kernel;
// // ************************
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,6,0)
static const struct proc_ops usb_monitor_fops = {
    // .owner = THIS_MODULE,
    .proc_open = usb_monitor_open,
    .proc_release = usb_monitor_release,
    .proc_read = usb_monitor_read,
    .proc_write = usb_monitor_write,
    .proc_poll = usb_monitor_poll,
    .proc_ioctl = usb_monitor_ioctl,
;
#else
static const struct file_operations usb_monitor_fops = {
    .owner = THIS_MODULE,
    .read = usb_monitor_read,
    .write = usb_monitor_write,
    .poll = usb_monitor_poll,
    .unlocked_ioctl = usb_monitor_ioctl,
};
#endif


/**
 * Writing data to the circular queue;
 *
 * @param status;
 * @param usb_dev;
 * @param OUT index;
 */
void write_message(char status,struct usb_device *usb_dev, OUT int *index){

    int tmp_index;

    LOGI("%s:%s\n", TAG, __func__);

    tmp_index = monitor->usb_message_index_write;
    monitor->message[tmp_index].kernel_time = ktime_to_ns(ktime_get());

    // Determine if the device name is empty to avoid crashing the program;
    if(usb_dev->product){
        printk("write_message %ld\n", strlen(usb_dev->product));
        memcpy(monitor->message[tmp_index].usb_name,usb_dev->product,strlen(usb_dev->product) );
    }else{
        memcpy(monitor->message[tmp_index].usb_name, "NULL", 4);
        printk("write_message get nothing\n");
    }
    // Record usb device plugging status;
    monitor->message[tmp_index].plug_flag = status;

    // Add one to the number of data in the circular queue
    if (monitor->usb_message_count < MESSAGE_BUFFER_SIZE){
        monitor->usb_message_count++;
    }

    // If the read address exceeds the maximum value of the circular queue address,
    // set to zero;
    monitor->usb_message_index_write++;
    if (monitor->usb_message_index_write >= MESSAGE_BUFFER_SIZE){
        monitor->usb_message_index_write = 0;
    }

    *index = tmp_index;
}


/**
 * Implementation of notifier callback function;
 *
 * @param self;
 * @param event;
 * @param dev;
 */
static int usb_notifier_callback(struct notifier_block *self, unsigned long event, void *dev) {

    struct usb_device *usb_dev = (struct usb_device*)dev;
    int index;

    // Get locked
    mutex_lock(&monitor->usb_monitor_mutex);

    switch (event) {
//         #define USB_DEVICE_ADD     0x0001
//         #define USB_DEVICE_REMOVE  0x0002
//         #define USB_BUS_ADD        0x0003
//         #define USB_BUS_REMOVE     0x0004

        case USB_DEVICE_ADD:
            write_message(1, usb_dev, &index);
            printk(KERN_INFO "The add device name is %s %d\n", monitor->message[index].usb_name,
            monitor->usb_message_count);
            // Wake up;
            wake_up_interruptible(&monitor->usb_monitor_queue);
            break;

        case USB_DEVICE_REMOVE:
            write_message(0, usb_dev, &index);
            printk(KERN_INFO "The remove device name is %s %d\n", monitor->message[index].usb_name, monitor->usb_message_count);
            // Wake up;
            wake_up_interruptible(&monitor->usb_monitor_queue);
            break;
        default:
            break;
    }
    
    // Unlock;
    mutex_unlock(&monitor->usb_monitor_mutex);

    return NOTIFY_OK;
}


/**
 * Initialization
 */
static int __init usb_monitor_init(void) { 

    // 向内核申请空间
    monitor = kzalloc(sizeof(struct usb_monitor_t), GFP_KERNEL);

    if (!monitor) {
        LOGE("%s:failed to kzalloc\n", TAG);
        return -ENOMEM;
    }
    // 初始化环形队列读写地址和大小 
    monitor->usb_message_count = 0;
    monitor->usb_message_index_read = 0;
    monitor->usb_message_index_write = 0;
    monitor->init_flag = "start the usb_monitor_init...\n";
    // 在/proc 下创建虚拟文件，用于 用户和内核交互，这里只有读的要求 
    proc_create("usb_monitor", 0644, NULL, &usb_monitor_fops);

    // 初始化等待队列，这里的作用为了让usb_monitor_read 没有数据的时候挂起，不让用户频繁调用
    init_waitqueue_head(&monitor->usb_monitor_queue);

    mutex_init(&monitor->usb_monitor_mutex);
    monitor->fb_notif.notifier_call = usb_notifier_callback;

    printk(KERN_INFO "Init USB hook.\n"); 
    // 注册USB状态改变通知
    usb_register_notify(&monitor->fb_notif);
    return 0;
}


/**
 * Exit
 */
static void __exit usb_monitor_exit(void)
{


    LOGI("%s:%s\n", TAG, __func__);

    remove_proc_entry("usb_monitor", NULL);

    usb_unregister_notify(&monitor->fb_notif); 
    printk(KERN_INFO "Remove USB hook\n");
    kfree(monitor);
}

module_init(usb_monitor_init);
module_exit(usb_monitor_exit);

MODULE_LICENSE("GPL");
