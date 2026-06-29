#include <linux/module.h> /* Needed by all modules */
#include <linux/kernel.h>
#include <linux/printk.h> /* Needed for pr_info() */
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h> //used for struct class
#include <linux/atomic.h>
#include <linux/uaccess.h>        //copy_to_user(), copy_from_user
#include <linux/utsname.h>        //get the hostname and the release version
#include <linux/cpumask.h>        //num_possible_cpus(), num_online_cpus()
#include <linux/cpu.h>            //cpu_data()
#include <linux/mm.h>             //si_meminfo()
#include <linux/time_namespace.h> //uptime
#include <linux/sched.h>

#define KFETCH_DEV_NAME "kfetch"
#define KFETCH_BUF_SIZE 2048
#define KFETCH_INFO_SIZE 128
#define KFETCH_DEV_NUM 1
#define CLOSED 0
#define OPENING 1

#define TITLE_COLOR "\033[0;33;1m"
#define LOGO_COLOR "\033[;38;5;214;1m"
#define RESET_COLOR "\033[0m"

static dev_t dev;
static struct cdev kfetch_cdev;
static struct class *kfetch_cls;
static atomic_t is_open = ATOMIC_INIT(CLOSED);
static int mask;
static char logo[7][64] = {LOGO_COLOR "         .-.        " RESET_COLOR,
                           LOGO_COLOR "        (.. |       " RESET_COLOR,
                           LOGO_COLOR "        <>  |       " RESET_COLOR,
                           LOGO_COLOR "       / --- \\      " RESET_COLOR,
                           LOGO_COLOR "      ( |   | |     " RESET_COLOR,
                           LOGO_COLOR "    |\\\\_)___/\\)/\\   " RESET_COLOR,
                           LOGO_COLOR "   <__)------(__/   " RESET_COLOR};
static size_t logo_width = 20;

static ssize_t kfetch_read(struct file *, char __user *, size_t, loff_t *);
static ssize_t kfetch_write(struct file *, const char __user *, size_t, loff_t *);
static ssize_t kfetch_read_system_info(char *, size_t);
static ssize_t kfetch_read_power_info(char *, size_t);
static int kfetch_open(struct inode *, struct file *);
static int kfetch_release(struct inode *, struct file *);
const static struct file_operations kfetch_ops = {
    .owner = THIS_MODULE,
    .read = kfetch_read,
    .write = kfetch_write,
    .open = kfetch_open,
    .release = kfetch_release,
};

static int __init kfetch_init(void)
{
    pr_info("Enter kfetch_mod\n");

    alloc_chrdev_region(&dev, 0, KFETCH_DEV_NUM, KFETCH_DEV_NAME);
    cdev_init(&kfetch_cdev, &kfetch_ops);
    cdev_add(&kfetch_cdev, dev, KFETCH_DEV_NUM);
    pr_info("Registering %s(major %d)\n", KFETCH_DEV_NAME, MAJOR(dev));

    kfetch_cls = class_create(KFETCH_DEV_NAME);
    device_create(kfetch_cls, NULL, dev, NULL, KFETCH_DEV_NAME);
    pr_info("Making the device file /dev/%s\n", KFETCH_DEV_NAME);
    return 0;
}

static void __exit kfetch_exit(void)
{
    device_destroy(kfetch_cls, dev);
    class_destroy(kfetch_cls);
    pr_info("Destroying the device /dev/%s\n", KFETCH_DEV_NAME);

    cdev_del(&kfetch_cdev);
    unregister_chrdev_region(dev, KFETCH_DEV_NUM);
    pr_info("Unregistering %s\n", KFETCH_DEV_NAME);

    pr_info("Leave kfetch_mod\n");
}

static ssize_t kfetch_read_system_info(char *buffer, size_t buffer_size)
{
    ssize_t ret = 0;
    char *infos[7] = {NULL};
    size_t curr_info = 0;

    char *hostname = utsname()->nodename;
    snprintf(buffer, buffer_size,
             "%*c" TITLE_COLOR "%s" RESET_COLOR "\n", (int)logo_width, ' ', hostname);

    // Seperate line
    infos[curr_info] = kmalloc(KFETCH_INFO_SIZE * sizeof(char), GFP_KERNEL);
    if (infos[curr_info] == NULL)
    {
        ret = -1;
        goto cleanup;
    }
    size_t sep_len = strnlen(hostname, KFETCH_INFO_SIZE - 1);
    memset(infos[curr_info], '-', sep_len);
    infos[curr_info][sep_len] = '\0';
    curr_info++;

    // Get kernel release
    if ((mask >> 0) & 1)
    {
        infos[curr_info] = kmalloc(KFETCH_INFO_SIZE * sizeof(char), GFP_KERNEL);
        if (infos[curr_info] == NULL)
        {
            ret = -1;
            goto cleanup;
        }
        snprintf(infos[curr_info], KFETCH_INFO_SIZE,
                 TITLE_COLOR "Kernel:" RESET_COLOR "\t%s", utsname()->release);
        curr_info++;
    }

    // Get CPU model name
    if ((mask >> 2) & 1)
    {
        infos[curr_info] = kmalloc(KFETCH_INFO_SIZE * sizeof(char), GFP_KERNEL);
        if (infos[curr_info] == NULL)
        {
            ret = -1;
            goto cleanup;
        }
        snprintf(infos[curr_info], KFETCH_INFO_SIZE,
                 TITLE_COLOR "CPU:" RESET_COLOR "\t%s", cpu_data(0).x86_model_id);
        curr_info++;
    }

    // Get the number of CPU cores
    if ((mask >> 1) & 1)
    {
        infos[curr_info] = kmalloc(KFETCH_INFO_SIZE * sizeof(char), GFP_KERNEL);
        if (infos[curr_info] == NULL)
        {
            ret = -1;
            goto cleanup;
        }
        snprintf(infos[curr_info], KFETCH_INFO_SIZE,
                 TITLE_COLOR "CPUs:" RESET_COLOR "\t%u / %u",
                 num_online_cpus(), num_possible_cpus());
        curr_info++;
    }

    // Get memory information
    if ((mask >> 3) & 1)
    {
        struct sysinfo mem;
        si_meminfo(&mem);
        size_t mem_free = (mem.freeram << PAGE_SHIFT) >> 20;
        size_t mem_total = (mem.totalram << PAGE_SHIFT) >> 20;
        infos[curr_info] = kmalloc(KFETCH_INFO_SIZE * sizeof(char), GFP_KERNEL);
        if (infos[curr_info] == NULL)
        {
            ret = -1;
            goto cleanup;
        }
        snprintf(infos[curr_info], KFETCH_INFO_SIZE,
                 TITLE_COLOR "Mem:" RESET_COLOR "\t%lu MB / %lu MB", mem_free, mem_total);
        curr_info++;
    }

    // Get the number of processes
    if ((mask >> 5) & 1)
    {
        struct task_struct *task;
        size_t proc_num = 0;
        rcu_read_lock();
        for_each_process(task)
        {
            proc_num += get_nr_threads(task);
        }
        rcu_read_unlock();
        infos[curr_info] = kmalloc(KFETCH_INFO_SIZE * sizeof(char), GFP_KERNEL);
        if (infos[curr_info] == NULL)
        {
            ret = -1;
            goto cleanup;
        }
        snprintf(infos[curr_info], KFETCH_INFO_SIZE,
                 TITLE_COLOR "Procs:" RESET_COLOR "\t%lu", proc_num);
        curr_info++;
    }

    // Get uptime
    if ((mask >> 4) & 1)
    {
        struct timespec64 uptime;
        ktime_get_boottime_ts64(&uptime);
        size_t uptime_min = uptime.tv_sec / 60;
        infos[curr_info] = kmalloc(KFETCH_INFO_SIZE * sizeof(char), GFP_KERNEL);
        if (infos[curr_info] == NULL)
        {
            ret = -1;
            goto cleanup;
        }
        snprintf(infos[curr_info], KFETCH_INFO_SIZE,
                 TITLE_COLOR "Uptime:" RESET_COLOR "\t%lu mins", uptime_min);
        curr_info++;
    }

    for (int i = 0; i < 7; i++)
    {
        strncat(buffer, logo[i], buffer_size - strlen(buffer) - 1);
        if (infos[i] != NULL)
        {
            strncat(buffer, infos[i], buffer_size - strlen(buffer) - 1);
        }
        strncat(buffer, "\n", buffer_size - strlen(buffer) - 1);
    }
    ret = strlen(buffer);

cleanup:
    for (int i = 0; i < 7; i++)
    {
        if (infos[i] != NULL)
        {
            kfree(infos[i]);
        }
    }
    return ret;
}

static ssize_t kfetch_read_power_info(char *buffer, size_t buffer_size)
{
    return 0;
}

static ssize_t kfetch_read(struct file *filp, char __user *buffer, size_t length, loff_t *offset)
{
    if (*offset > 0)
    {
        return 0;
    }
    ssize_t ret = 0;
    char *kfetch_buf = kmalloc(KFETCH_BUF_SIZE * sizeof(char), GFP_KERNEL);
    if (kfetch_buf == NULL)
    {
        ret = -1;
        goto cleanup;
    }

    if (kfetch_read_system_info(kfetch_buf, KFETCH_BUF_SIZE) < 0)
    {
        ret = -1;
        goto cleanup;
    }

    ret = strlen(kfetch_buf);
    if (kfetch_read_power_info(kfetch_buf + ret, KFETCH_BUF_SIZE - ret) < 0)
    {
        ret = -1;
        goto cleanup;
    }

    ret = strlen(kfetch_buf);
    if (copy_to_user(buffer, kfetch_buf, ret))
    {
        pr_alert("Failed to copy data to user");
        ret = -1;
        goto cleanup;
    }

    /* cleaning up */
cleanup:
    kfree(kfetch_buf);
    kfetch_buf = NULL;

    if (ret >= 0)
    {
        *offset += ret;
    }
    return ret;
}

static ssize_t kfetch_write(struct file *filp, const char __user *buffer, size_t length, loff_t *offset)
{
    pr_info("kfetch_write is running\n");
    if (copy_from_user(&mask, buffer, length))
    {
        pr_alert("Failed to copy data from user");
        return -1;
    }
    return 0;
}

static int kfetch_open(struct inode *inode, struct file *filp)
{
    if (atomic_cmpxchg(&is_open, CLOSED, OPENING) == OPENING)
    {
        return -1;
    }
    pr_info("Opening /dev/%s\n", KFETCH_DEV_NAME);
    return 0;
}

static int kfetch_release(struct inode *inode, struct file *filp)
{
    pr_info("Close /dev/%s\n", KFETCH_DEV_NAME);
    atomic_set(&is_open, CLOSED);
    return 0;
}

module_init(kfetch_init);
module_exit(kfetch_exit);
MODULE_LICENSE("GPL");