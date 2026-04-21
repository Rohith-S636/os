#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/workqueue.h>

#define IOCTL_REGISTER_PID _IOW('a', 'a', int)

#define SOFT_LIMIT (20 * 1024 * 1024)   // 50 MB
#define HARD_LIMIT (40 * 1024 * 1024)  // 40 MB

static int monitored_pid = -1;
static struct delayed_work monitor_work;

// ================= MEMORY CHECK =================
static void check_memory(struct work_struct *work) {
    struct task_struct *task;
    struct mm_struct *mm;
    unsigned long rss;

    if (monitored_pid == -1)
        return;

    task = pid_task(find_vpid(monitored_pid), PIDTYPE_PID);
    if (!task)
        return;

    mm = task->mm;
    if (!mm)
        return;

    rss = get_mm_rss(mm) << PAGE_SHIFT;

    printk(KERN_INFO "[Monitor] PID %d memory: %lu bytes\n",
           monitored_pid, rss);

    if (rss > SOFT_LIMIT) {
        printk(KERN_WARNING "[Monitor] PID %d exceeded SOFT limit\n",
               monitored_pid);
    }

    if (rss > HARD_LIMIT) {
        printk(KERN_ERR "[Monitor] PID %d exceeded HARD limit → killing\n",
               monitored_pid);
        send_sig(SIGKILL, task, 0);
        monitored_pid = -1;
        return;
    }

    schedule_delayed_work(&monitor_work, msecs_to_jiffies(1000));
}

// ================= IOCTL =================
static long monitor_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    int pid;

    if (cmd == IOCTL_REGISTER_PID) {
        if (copy_from_user(&pid, (int *)arg, sizeof(int)))
            return -EFAULT;

        monitored_pid = pid;

        printk(KERN_INFO "[Monitor] Registered PID: %d\n", pid);

        schedule_delayed_work(&monitor_work, msecs_to_jiffies(1000));
    }

    return 0;
}

// ================= FILE OPS =================
static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

// ================= DEVICE =================
static struct miscdevice monitor_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "container_monitor",
    .fops = &fops,
};

// ================= INIT =================
static int __init monitor_init(void) {
    misc_register(&monitor_device);
    INIT_DELAYED_WORK(&monitor_work, check_memory);

    printk(KERN_INFO "[Monitor] Module Loaded\n");
    return 0;
}

// ================= EXIT =================
static void __exit monitor_exit(void) {
    cancel_delayed_work_sync(&monitor_work);
    misc_deregister(&monitor_device);

    printk(KERN_INFO "[Monitor] Module Unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
