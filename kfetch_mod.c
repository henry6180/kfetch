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
#include <linux/cpumask.h>        //num_present_cpus(), num_online_cpus()
#include <linux/cpu.h>            //cpu_data()
#include <linux/mm.h>             //si_meminfo()
#include <linux/time_namespace.h> //uptime
#include <linux/sched.h>

#include <linux/kthread.h>
#include <linux/delay.h>
#include <asm/cpu_device_id.h>
#include <linux/mutex.h>
#include <linux/hashtable.h>
#include <linux/sort.h>
#include <linux/jiffies.h>
#include <linux/version.h>

#define KFETCH_DEV_NAME "kfetch"
#define KFETCH_BUF_SIZE 2048
#define KFETCH_INFO_SIZE 128
#define KFETCH_LOGO_WIDTH 20
#define KFETCH_DEV_NUM 1
#define CLOSED 0
#define OPENING 1

#define AMD_MSR_RAPL_POWER_UNIT 0xC0010299
#define MSR_CORE_ENERGY_STATUS 0xC001029A
#define MSR_PACKAGE_ENERGY_STATUS 0xC001029B
#define AMD_ENERGY_UNIT_MASK 0x01F00

#define TITLE_COLOR "\033[0;33;1m"
#define LOGO_COLOR "\033[;38;5;214;1m"
#define RESET_COLOR "\033[0m"

static int mask;
static char logo[7][64] = {LOGO_COLOR "         .-.        " RESET_COLOR,
                           LOGO_COLOR "        (.. |       " RESET_COLOR,
                           LOGO_COLOR "        <>  |       " RESET_COLOR,
                           LOGO_COLOR "       / --- \\      " RESET_COLOR,
                           LOGO_COLOR "      ( |   | |     " RESET_COLOR,
                           LOGO_COLOR "    |\\\\_)___/\\)/\\   " RESET_COLOR,
                           LOGO_COLOR "   <__)------(__/   " RESET_COLOR};

struct Counters
{
    unsigned long long curr;
    unsigned long long prev;
};
static unsigned long long energy_unit;
static size_t sample_rate_ms;
static struct task_struct *accumulator;
static struct mutex counter_lock;
static struct Counters *counters_core;
static size_t core_num;
static struct Counters *counters_pkg;
static size_t pkg_num;

static struct mutex uj_lock;
static struct Counters *core_uj;
static struct Counters *pkg_uj;
static size_t calculate_rate_ms;
static struct task_struct *calculator;

struct Timeinfo
{
    pid_t pid;
    u64 utime;
    u64 stime;
    u64 timestamp;
    u64 cpu_ratio; // [0, 1000]
    int cpu_id;
    struct hlist_node node;
};
struct Powerinfo
{
    pid_t pid;
    char name[TASK_COMM_LEN];
    unsigned long long watt;
};
DEFINE_HASHTABLE(timeinfos, 16);
static size_t timeinfo_num;
static struct mutex timeinfo_lock;
static size_t record_rate_ms;
static struct task_struct *recorder;

static ssize_t kfetch_read(struct file *, char __user *, size_t, loff_t *);
static ssize_t kfetch_write(struct file *, const char __user *, size_t, loff_t *);
static void calculate_cpu_topology(void);
static struct Counters *init_counter(size_t);
static void update_counter(struct Counters *, unsigned, size_t);
static void init_timeinfos(void);
static void update_timeinfo(struct task_struct *);
static void delete_timeinfos(void);
static int compare_power(const void *, const void *);
static int kfetch_record_runtime(void *);
static int kfetch_calculate_power(void *);
static int kfetch_accumulate_energy(void *);
static ssize_t kfetch_read_system_info(char *, size_t);
static ssize_t kfetch_read_power_info(char *, size_t);
static int kfetch_open(struct inode *, struct file *);
static int kfetch_release(struct inode *, struct file *);

static dev_t dev;
static struct cdev kfetch_cdev;
static struct class *kfetch_cls;
static atomic_t is_open = ATOMIC_INIT(CLOSED);
static const struct file_operations kfetch_ops = {
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

    // Init accumulator
    mutex_init(&counter_lock);
    calculate_cpu_topology();
    counters_core = init_counter(core_num);
    counters_pkg = init_counter(pkg_num);
    if (counters_core == NULL || counters_pkg == NULL)
    {
        pr_info("Failed to allocate space to counters_core or counters_pkg.\n");
        return -1;
    }
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
    rdmsrq_safe(AMD_MSR_RAPL_POWER_UNIT, &energy_unit);
#else
    rdmsrl_safe(AMD_MSR_RAPL_POWER_UNIT, &energy_unit);
#endif
    energy_unit = (energy_unit & AMD_ENERGY_UNIT_MASK) >> 8;
    sample_rate_ms = 100;
    accumulator = kthread_run(kfetch_accumulate_energy, NULL, "kfetch_accumulate_power");

    // Init calculator
    mutex_init(&uj_lock);
    core_uj = init_counter(core_num);
    pkg_uj = init_counter(1);
    if (core_uj == NULL || pkg_uj == NULL)
    {
        pr_info("Failed to allocate space to core_uj or pkg_uj.\n");
        return -1;
    }
    calculate_rate_ms = 1000;
    calculator = kthread_run(kfetch_calculate_power, NULL, "kfetch_calculate_power");

    // Init recorder
    mutex_init(&timeinfo_lock);
    timeinfo_num = 0;
    init_timeinfos();
    if (timeinfo_num == 0)
    {
        pr_info("Failed to initialize the hash table of timeinfos\n");
        return -1;
    }
    record_rate_ms = 500;
    recorder = kthread_run(kfetch_record_runtime, NULL, "kfetch_record_runtime");
    return 0;
}

static void __exit kfetch_exit(void)
{
    // Stop accumulating energy
    if (accumulator != NULL)
    {
        kthread_stop(accumulator);
        pr_info("Stop probing cpu energy\n");
    }
    mutex_destroy(&counter_lock);
    kfree(counters_core);
    kfree(counters_pkg);
    // Stop calculating power
    if (calculator != NULL)
    {
        kthread_stop(calculator);
        pr_info("Stop calculating cpu power consumption\n");
    }
    mutex_destroy(&uj_lock);
    kfree(core_uj);
    kfree(pkg_uj);
    // Stop recording timeinfos
    if (recorder != NULL)
    {
        kthread_stop(recorder);
        pr_info("Stop recording timeinfos of processes\n");
    }
    delete_timeinfos();
    mutex_destroy(&timeinfo_lock);

    device_destroy(kfetch_cls, dev);
    class_destroy(kfetch_cls);
    pr_info("Destroying the device /dev/%s\n", KFETCH_DEV_NAME);

    cdev_del(&kfetch_cdev);
    unregister_chrdev_region(dev, KFETCH_DEV_NUM);
    pr_info("Unregistering %s\n", KFETCH_DEV_NAME);

    pr_info("Leave kfetch_mod\n");
}

static void calculate_cpu_topology()
{
    struct cpumask *visited_cores = kmalloc(sizeof(struct cpumask), GFP_KERNEL);
    struct cpumask *visited_pkgs = kmalloc(sizeof(struct cpumask), GFP_KERNEL);
    cpumask_clear(visited_cores);
    cpumask_clear(visited_pkgs);

    size_t cpu;
    for_each_present_cpu(cpu)
    {
        if (!cpumask_test_cpu(cpu, visited_cores))
        {
            core_num++;
            // topology_sibling_cpumask: the mask where the core and its siblings are set.
            cpumask_or(visited_cores, visited_cores, topology_sibling_cpumask(cpu));
        }
        if (!cpumask_test_cpu(cpu, visited_pkgs))
        {
            pkg_num++;
            // topology_core_cpumask: the mask where the cores on the same package are set.
            cpumask_or(visited_pkgs, visited_pkgs, topology_core_cpumask(cpu));
        }
    }

    kfree(visited_cores);
    kfree(visited_pkgs);
}

static struct Counters *init_counter(size_t n)
{
    struct Counters *counter = kmalloc(n * sizeof(struct Counters), GFP_KERNEL);
    if (counter == NULL)
    {
        return NULL;
    }
    for (int i = 0; i < n; i++)
    {
        counter[i].curr = 0;
        counter[i].prev = 0;
    }
    return counter;
}

static void update_counter(struct Counters *counter, unsigned cpu, size_t msr_no)
{
    // read msr
    unsigned long long value;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
    rdmsrq_safe_on_cpu(cpu, msr_no, &value);
#else
    rdmsrl_safe_on_cpu(cpu, msr_no, &value);
#endif
    // unsigned long long -> unsigned int
    value &= UINT_MAX;

    // update counter
    mutex_lock(&counter_lock);
    if (value >= counter->prev)
    {
        counter->curr += value - counter->prev;
    }
    else
    {
        counter->curr += UINT_MAX - counter->prev + value;
    }
    counter->prev = value;
    mutex_unlock(&counter_lock);
}

static void init_timeinfos()
{
    struct task_struct *task;
    rcu_read_lock();
    mutex_lock(&timeinfo_lock);
    for_each_process(task)
    {
        struct Timeinfo *timeinfo = kmalloc(sizeof(struct Timeinfo), GFP_KERNEL);
        if (timeinfo == NULL)
        {
            mutex_unlock(&timeinfo_lock);
            delete_timeinfos();
            timeinfo_num = 0;
            return;
        }
        timeinfo->pid = task->pid;
        timeinfo->utime = task->utime;
        timeinfo->stime = task->stime;
        timeinfo->timestamp = ktime_get_ns();
        timeinfo->cpu_ratio = 0;
        timeinfo->cpu_id = task_cpu(task);
        hash_add(timeinfos, &timeinfo->node, timeinfo->pid);
        timeinfo_num++;
    }
    mutex_unlock(&timeinfo_lock);
    rcu_read_unlock();
}

static void update_timeinfo(struct task_struct *curr_task)
{
    struct Timeinfo *timeinfo;
    mutex_lock(&timeinfo_lock);
    bool found = false;
    hash_for_each_possible(timeinfos, timeinfo, node, curr_task->pid)
    {
        if (timeinfo->pid == curr_task->pid)
        {
            // struct Timeinfo old = *timeinfo;
            u64 utime_old = timeinfo->utime;
            u64 stime_old = timeinfo->stime;
            u64 timestamp_old = timeinfo->timestamp;
            timeinfo->utime = curr_task->utime;
            timeinfo->stime = curr_task->stime;
            timeinfo->timestamp = ktime_get_ns();
            timeinfo->cpu_id = task_cpu(curr_task);
            // the process is recreated
            if (curr_task->start_boottime < timestamp_old)
            {
                timeinfo->cpu_ratio = 0;
            }
            else
            {
                u64 utime_diff = timeinfo->utime - utime_old;
                u64 stime_diff = timeinfo->stime - stime_old;
                u64 time_diff_ns = timeinfo->timestamp - timestamp_old;
                //jiffies64_to_nsecs(utime_diff + stime_diff)
                timeinfo->cpu_ratio = div64_ul(1000UL * (utime_diff + stime_diff), time_diff_ns);
            }
            found = true;
            break;
        }
    }
    // the process is newly created
    if (!found)
    {
        struct Timeinfo *timeinfo = kmalloc(sizeof(struct Timeinfo), GFP_KERNEL);
        if (timeinfo == NULL)
        {
            mutex_unlock(&timeinfo_lock);
            delete_timeinfos();
            timeinfo_num = 0;
            return;
        }
        timeinfo->pid = curr_task->pid;
        timeinfo->utime = curr_task->utime;
        timeinfo->stime = curr_task->stime;
        timeinfo->timestamp = ktime_get_ns();
        timeinfo->cpu_ratio = 0;
        timeinfo->cpu_id = task_cpu(curr_task);
        hash_add(timeinfos, &timeinfo->node, timeinfo->pid);
        timeinfo_num++;
    }
    mutex_unlock(&timeinfo_lock);
}

static void delete_timeinfos()
{
    struct Timeinfo *timeinfo;
    struct hlist_node *tmp;
    int bucket;
    mutex_lock(&timeinfo_lock);
    hash_for_each_safe(timeinfos, bucket, tmp, timeinfo, node)
    {
        hash_del(&timeinfo->node);
        kfree(timeinfo);
    }
    mutex_unlock(&timeinfo_lock);
}

static int compare_power(const void *lhs, const void *rhs)
{
    const struct Powerinfo *powerinfo_lhs = (const struct Powerinfo *)lhs;
    const struct Powerinfo *powerinfo_rhs = (const struct Powerinfo *)rhs;
    if (powerinfo_lhs->watt < powerinfo_rhs->watt)
    {
        return 1;
    }
    if (powerinfo_lhs->watt == powerinfo_rhs->watt)
    {
        return 0;
    }
    return -1;
}

static int kfetch_record_runtime(void *args)
{
    pr_info("kfetch_record_runtime is running.\n");
    while (!kthread_should_stop())
    {
        struct task_struct *task;
        rcu_read_lock();
        for_each_process(task)
        {
            update_timeinfo(task);
            if (timeinfo_num == 0)
            {
                pr_info("Failed to update the hash table of timeinfos\n");
                return -1;
            }
        }
        rcu_read_unlock();

        if (kthread_should_stop())
        {
            break;
        }

        msleep_interruptible(record_rate_ms);
    }
    return 0;
}

static int kfetch_calculate_power(void *args)
{
    pr_info("kfetch_calculate_power is running.\n");
    unsigned long long *copy = kmalloc(core_num * sizeof(unsigned long long), GFP_KERNEL);
    while (!kthread_should_stop())
    {
        unsigned long long pkg_sum = 0;
        mutex_lock(&counter_lock);
        for (int i = 0; i < core_num; i++)
        {
            if (cpu_online(i))
            {
                copy[i] = counters_core[i].curr;
            }
        }
        for (int i = 0; i < pkg_num; i++)
        {
            pkg_sum += counters_pkg[i].curr;
        }
        mutex_unlock(&counter_lock);

        mutex_lock(&uj_lock);
        for (int i = 0; i < core_num; i++)
        {
            if (cpu_online(i))
            {
                core_uj[i].prev = core_uj[i].curr;
                core_uj[i].curr = div64_ul(copy[i] * 1000000UL, BIT(energy_unit));
            }
        }
        pkg_uj->prev = pkg_uj->curr;
        pkg_uj->curr = div64_ul(pkg_sum * 1000000UL, BIT(energy_unit));
        mutex_unlock(&uj_lock);

        if (kthread_should_stop())
        {
            break;
        }

        msleep_interruptible(calculate_rate_ms);
    }
    kfree(copy);
    return 0;
}

static int kfetch_accumulate_energy(void *args)
{
    pr_info("kfetch_accumulate_energy is running.\n");
    while (!kthread_should_stop())
    {
        // core
        for (unsigned cpu = 0; cpu < core_num; cpu++)
        {
            if (cpu_online(cpu))
            {
                update_counter(counters_core + cpu, cpu, MSR_CORE_ENERGY_STATUS);
            }
        }
        // package
        for (unsigned pkg = 0; pkg < pkg_num; pkg++)
        {
            unsigned cpu = cpumask_first_and(
                cpu_online_mask,
                topology_die_cpumask(core_num / pkg_num * pkg));
            update_counter(counters_pkg + pkg, cpu, MSR_PACKAGE_ENERGY_STATUS);
        }

        if (kthread_should_stop())
        {
            break;
        }

        msleep_interruptible(sample_rate_ms);
    }

    return 0;
}

static ssize_t kfetch_read_system_info(char *buffer, size_t buffer_size)
{
    ssize_t ret = 0;
    char *infos[7] = {NULL};
    size_t curr_info = 0;

    char *hostname = utsname()->nodename;
    snprintf(buffer, buffer_size,
             "%*c" TITLE_COLOR "%s" RESET_COLOR "\n", KFETCH_LOGO_WIDTH, ' ', hostname);

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
                 num_online_cpus(), num_present_cpus());
        curr_info++;
    }

    // Get memory information
    if ((mask >> 3) & 1)
    {
        struct sysinfo mem;
        si_meminfo(&mem);
        size_t cached_pages = mem.bufferram + global_node_page_state(NR_FILE_PAGES);
        size_t mem_avail = ((mem.freeram + cached_pages) << PAGE_SHIFT) >> 20;
        size_t mem_total = (mem.totalram << PAGE_SHIFT) >> 20;
        infos[curr_info] = kmalloc(KFETCH_INFO_SIZE * sizeof(char), GFP_KERNEL);
        if (infos[curr_info] == NULL)
        {
            ret = -1;
            goto cleanup;
        }
        snprintf(infos[curr_info], KFETCH_INFO_SIZE,
                 TITLE_COLOR "Mem:" RESET_COLOR "\t%lu MB / %lu MB", mem_avail, mem_total);
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
    if (((mask >> 6) & 1) == 0)
    {
        return strlen(buffer);
    }

    size_t sep_len = 64;
    char seperate_line[KFETCH_INFO_SIZE] = "";
    memset(seperate_line, '=', sep_len - 1);
    seperate_line[sep_len - 1] = '\n';
    seperate_line[sep_len] = '\0';

    unsigned long long pkg_uw = 1000 * div64_ul(pkg_uj->curr - pkg_uj->prev, calculate_rate_ms);
    unsigned long long *core_uws = kmalloc(core_num * sizeof(unsigned long long), GFP_KERNEL);
    unsigned long long core_uw = 0;
    for (int i = 0; i < core_num; i++)
    {
        if (cpu_online(i))
        {
            // core_uw += 1000 * div64_ul(core_uj[i].curr - core_uj[i].prev, calculate_rate_ms);
            core_uws[i] = 1000UL * div64_ul(core_uj[i].curr - core_uj[i].prev, calculate_rate_ms);
            core_uw += core_uws[i];
        }
    }
    if (core_uw > __LONG_LONG_MAX__ || pkg_uw > __LONG_LONG_MAX__)
    {
        pr_info("Power of cpu cores or packages overflow\n");
        kfree(core_uws);
        return -1;
    }

    if (core_uw == 0 || pkg_uw == 0)
    {
        snprintf(buffer, buffer_size, "%sPower of cpu cores or packages == 0.\n", seperate_line);
    }
    else
    {
        unsigned core_w_rem = 0;
        div_u64_rem(core_uw, 1000000UL, &core_w_rem);
        unsigned pkg_w_rem = 0;
        div_u64_rem(pkg_uw, 1000000UL, &pkg_w_rem);
        snprintf(buffer, buffer_size,
                 "%spkg power: %llu.%u W\tcore power: %llu.%u W.\n",
                 seperate_line, div64_ul(pkg_uw, 1000000UL), pkg_w_rem / 1000,
                 div64_ul(core_uw, 1000000UL), core_w_rem / 1000);
    }

    if ((mask >> 7) & 1)
    {
        struct task_struct *task;
        struct Timeinfo *timeinfo;
        struct Powerinfo *powerinfos = kmalloc(timeinfo_num * sizeof(struct Powerinfo), GFP_KERNEL);
        int count = 0;
        rcu_read_lock();
        for_each_process(task)
        {
            hash_for_each_possible(timeinfos, timeinfo, node, task->pid)
            {
                if (timeinfo->pid == task->pid)
                {
                    powerinfos[count].pid = task->pid;
                    get_task_comm(powerinfos[count].name, task);
                    powerinfos[count].watt = div64_ul((core_uws[timeinfo->cpu_id] * timeinfo->cpu_ratio), 1000UL);
                    count++;
                    break;
                }
            }
        }
        rcu_read_unlock();
        sort(powerinfos, count, sizeof(struct Powerinfo), compare_power, NULL);
        char buf[KFETCH_INFO_SIZE] = "";
        // strncat(buffer, seperate_line, buffer_size - sep_len - 1);
        strncat(buffer, "pid\tprocess name\t\twatt(uw)\n", buffer_size - strlen(buffer) - 1);
        for (int i = 0; i < min(10, count); i++)
        {
            snprintf(buf, KFETCH_INFO_SIZE, "%d\t%s\t\t%llu\n", powerinfos[i].pid, powerinfos[i].name, powerinfos[i].watt);
            strncat(buffer, buf, buffer_size - strlen(buffer) - 1);
        }
        strncat(buffer, seperate_line, buffer_size - sep_len - 1);
        kfree(powerinfos);
    }

    return strlen(buffer);
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