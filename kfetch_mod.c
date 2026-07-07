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
#define KFETCH_INFO_SIZE 80
#define KFETCH_DEV_NUM 1
#define CLOSED 0
#define OPENING 1
#define KFETCH_DEFAULT_SAMPLE_RATE_MS 100
#define KFETCH_DEFAULT_CALCULATE_RATE_MS 1000
#define KFETCH_DEFAULT_RECORD_RATE_MS 1000

#define AMD_MSR_RAPL_POWER_UNIT 0xC0010299
#define MSR_CORE_ENERGY_STATUS 0xC001029A
#define MSR_PACKAGE_ENERGY_STATUS 0xC001029B
#define AMD_ENERGY_UNIT_MASK 0x01F00

#define TITLE_COLOR "\033[0;33;1m"
#define LOGO_COLOR "\033[;38;5;214;1m"
#define RESET_COLOR "\033[0m"

static size_t mask;
static char logo[7][64] = {LOGO_COLOR "         .-.        " RESET_COLOR,
                           LOGO_COLOR "        (.. |       " RESET_COLOR,
                           LOGO_COLOR "        <>  |       " RESET_COLOR,
                           LOGO_COLOR "       / --- \\      " RESET_COLOR,
                           LOGO_COLOR "      ( |   | |     " RESET_COLOR,
                           LOGO_COLOR "    |\\\\_)___/\\)/\\   " RESET_COLOR,
                           LOGO_COLOR "   <__)------(__/   " RESET_COLOR};
static int kfetch_logo_width = 20;

struct energy_counter
{
    u64 curr;
    u64 prev;
};
static struct mutex counter_lock;
static u32 core_num;
static u32 pkg_num;
static struct energy_counter *counters_core;
static struct energy_counter *counters_pkg;
static u32 sample_rate_ms;
static struct task_struct *accumulator;

static struct mutex joule_lock;
static u64 **core_uj; // [core_num][old, new]
static u64 *pkg_uj;   // [old, new]
static u64 energy_unit;
static u32 calculate_rate_ms;
static struct task_struct *calculator;

struct proc_timeinfo
{
    pid_t pid;
    u64 utime;
    u64 stime;
    u64 timestamp;
    u32 cpu_ratio; // [0 ~ 1000]
    u32 core_id;
    struct hlist_node node;
};
DEFINE_HASHTABLE(timeinfo_table, 16);
static struct mutex timeinfo_lock;
static u32 timeinfo_num;
static u32 record_rate_ms;
static struct task_struct *recorder;

struct proc_powerinfo
{
    pid_t pid;
    char *name;
    u64 mw;
};

static ssize_t kfetch_read(struct file *, char __user *, size_t, loff_t *);
static ssize_t kfetch_write(struct file *, const char __user *, size_t, loff_t *);
static void calculate_cpu_topology(void);
static inline u32 get_core_id(unsigned int);
static void update_counter(struct energy_counter *, unsigned, size_t);
static void init_timeinfos(void);
static void update_timeinfo(struct proc_timeinfo *, u64);
static void delete_timeinfos(void);
static struct task_struct *find_task(pid_t);
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
    core_num = 0;
    pkg_num = 0;
    calculate_cpu_topology();
    if (core_num == 0 || pkg_num == 0)
    {
        pr_info("Failed to calculate the number of cpu cores or cpu packages\n");
        return -1;
    }
    counters_core = kzalloc(core_num * sizeof(struct energy_counter), GFP_KERNEL);
    counters_pkg = kzalloc(pkg_num * sizeof(struct energy_counter), GFP_KERNEL);
    if (counters_core == NULL || counters_pkg == NULL)
    {
        pr_info("Failed to allocate space to counters_core or counters_pkg.\n");
        return -1;
    }
    sample_rate_ms = KFETCH_DEFAULT_SAMPLE_RATE_MS;

    // Init calculator
    mutex_init(&joule_lock);
    core_uj = kzalloc(core_num * sizeof(u64 *), GFP_KERNEL);
    pkg_uj = kzalloc(2 * sizeof(u64), GFP_KERNEL);
    if (core_uj == NULL || pkg_uj == NULL)
    {
        pr_info("Failed to allocate space to core_uj or pkg_uj.\n");
        return -1;
    }
    for (int i = 0; i < core_num; i++)
    {
        core_uj[i] = kzalloc(2 * sizeof(u64), GFP_KERNEL);
        if (core_uj[i] == NULL)
        {
            pr_info("Failed to allocate space to core_uj.\n");
            return -1;
        }
    }
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
    rdmsrq_safe(AMD_MSR_RAPL_POWER_UNIT, &energy_unit);
#else
    rdmsrl_safe(AMD_MSR_RAPL_POWER_UNIT, &energy_unit);
#endif
    energy_unit = (energy_unit & AMD_ENERGY_UNIT_MASK) >> 8;
    calculate_rate_ms = KFETCH_DEFAULT_CALCULATE_RATE_MS;

    // Init recorder
    mutex_init(&timeinfo_lock);
    timeinfo_num = 0;
    init_timeinfos();
    if (timeinfo_num == 0)
    {
        pr_info("Failed to initialize the hash table of timeinfos\n");
        return -1;
    }
    record_rate_ms = KFETCH_DEFAULT_RECORD_RATE_MS;

    // Run the kthreads
    accumulator = kthread_run(kfetch_accumulate_energy, NULL, "kfetch_accumulate_power");
    calculator = kthread_run(kfetch_calculate_power, NULL, "kfetch_calculate_power");
    recorder = kthread_run(kfetch_record_runtime, NULL, "kfetch_record_runtime");

    return 0;
}

static void __exit kfetch_exit(void)
{
    // Clear up accumulator-related resources.
    if (accumulator != NULL)
    {
        kthread_stop(accumulator);
        pr_info("Stopping kfetch_accumulate_energy kthread.\n");
    }
    kfree(counters_core);
    kfree(counters_pkg);
    mutex_destroy(&counter_lock);

    // Clear up calculator-related resources.
    if (calculator != NULL)
    {
        kthread_stop(calculator);
        pr_info("Stopping calculating cpu power consumption\n");
    }
    for (int i = 0; i < core_num; i++)
    {
        kfree(core_uj[i]);
    }
    kfree(core_uj);
    kfree(pkg_uj);
    mutex_destroy(&joule_lock);

    // Clear up recorder-related resources.
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
    if (visited_cores == NULL || visited_pkgs == NULL)
    {
        return;
    }
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

static inline u32 get_core_id(unsigned int cpu_id)
{
    return topology_physical_package_id(cpu_id) * (core_num / pkg_num) + topology_core_id(cpu_id);
}

static void update_counter(struct energy_counter *counter, unsigned cpu, size_t msr_no)
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
    u32 task_num = 0;
    rcu_read_lock();
    for_each_process(task)
    {
        task_num++;
    }
    rcu_read_unlock();

    u32 task_num_with_guard = task_num + task_num / 2;
    struct proc_timeinfo **timeinfos = kzalloc(sizeof(struct proc_timeinfo *), GFP_KERNEL);
    if (timeinfos == NULL)
    {
        return;
    }
    for (int i = 0; i < task_num_with_guard; i++)
    {
        timeinfos[i] = kzalloc(sizeof(struct proc_timeinfo), GFP_KERNEL);
        if (timeinfos[i] == NULL)
        {
            for (int j = 0; j < i; j++)
            {
                kfree(timeinfos[i]);
            }
            kfree(timeinfos);
            return;
        }
    }

    u32 info_num = 0;
    rcu_read_lock();
    for_each_process(task)
    {
        timeinfos[info_num]->pid = task->pid;
        timeinfos[info_num]->utime = task->utime;
        timeinfos[info_num]->stime = task->stime;
        timeinfos[info_num]->timestamp = ktime_get_ns();
        timeinfos[info_num]->core_id = get_core_id(task_cpu(task));
        info_num++;
        if (info_num == task_num_with_guard)
        {
            break;
        }
    }
    rcu_read_unlock();

    if (info_num < task_num_with_guard)
    {
        for (int i = info_num; i < task_num_with_guard; i++)
        {
            kfree(timeinfos[i]);
        }
    }
    else
    {
        pr_info("Warning: The number of traversed tasks > task nums "
                "with guard when initializing timeinfos\n");
    }
    for (int i = 0; i < info_num; i++)
    {
        hash_add(timeinfo_table, &timeinfos[i]->node, timeinfos[i]->pid);
        timeinfo_num++;
    }
}

static void update_timeinfo(struct proc_timeinfo *timeinfo_new, u64 boottime)
{
    // Find the old timeinfo
    struct proc_timeinfo *timeinfo = NULL;
    struct proc_timeinfo *temp;
    mutex_lock(&timeinfo_lock);
    hash_for_each_possible(timeinfo_table, temp, node, timeinfo_new->pid)
    {
        if (temp->pid == timeinfo_new->pid)
        {
            timeinfo = temp;
            break;
        }
    }
    mutex_unlock(&timeinfo_lock);

    // The task is newly created.
    if (timeinfo == NULL)
    {
        timeinfo = kzalloc(sizeof(struct proc_timeinfo), GFP_KERNEL);
        if (timeinfo == NULL)
        {
            pr_info("Failed to allocate space of timeinfo for new process when updating timeinfo\n");
            return;
        }
        timeinfo->pid = timeinfo_new->pid;
        timeinfo->utime = timeinfo_new->utime;
        timeinfo->stime = timeinfo_new->stime;
        timeinfo->timestamp = timeinfo_new->timestamp;
        timeinfo->core_id = timeinfo_new->core_id;
        mutex_lock(&timeinfo_lock);
        hash_add(timeinfo_table, &timeinfo->node, timeinfo->pid);
        timeinfo_num++;
        mutex_unlock(&timeinfo_lock);
    }
    else
    {
        mutex_lock(&timeinfo_lock);
        struct proc_timeinfo timeinfo_old = *timeinfo;
        timeinfo->utime = timeinfo_new->utime;
        timeinfo->stime = timeinfo_new->stime;
        timeinfo->timestamp = timeinfo_new->timestamp;
        timeinfo->core_id = timeinfo_new->core_id;
        // the pid is reused by another task
        if (boottime > timeinfo_old.timestamp)
        {
            timeinfo->cpu_ratio = 0;
        }
        else
        {
            u64 utime_diff = timeinfo->utime - timeinfo_old.utime;                             // ns
            u64 stime_diff = timeinfo->stime - timeinfo_old.stime;                             // ns
            u64 time_diff_us = div64_ul(timeinfo->timestamp - timeinfo_old.timestamp, 1000UL); // us
            timeinfo->cpu_ratio = div64_u64(utime_diff + stime_diff, time_diff_us);            // [0 ~ 1000]
        }
        mutex_unlock(&timeinfo_lock);
    }
}

static void delete_timeinfos()
{
    struct proc_timeinfo *timeinfo;
    struct hlist_node *tmp;
    int bucket;
    mutex_lock(&timeinfo_lock);
    hash_for_each_safe(timeinfo_table, bucket, tmp, timeinfo, node)
    {
        hash_del(&timeinfo->node);
        kfree(timeinfo);
    }
    mutex_unlock(&timeinfo_lock);
}

static struct task_struct *find_task(pid_t pid)
{
    rcu_read_lock();
    struct task_struct *ret = NULL;
    struct task_struct *task;
    for_each_process(task)
    {
        if (task->pid == pid)
        {
            ret = task;
            break;
        }
    }
    rcu_read_unlock();
    return ret;
}

static int compare_power(const void *lhs, const void *rhs)
{
    const struct proc_powerinfo *powerinfo_lhs = (const struct proc_powerinfo *)lhs;
    const struct proc_powerinfo *powerinfo_rhs = (const struct proc_powerinfo *)rhs;
    if (powerinfo_lhs->mw < powerinfo_rhs->mw)
    {
        return 1;
    }
    if (powerinfo_lhs->mw == powerinfo_rhs->mw)
    {
        return 0;
    }
    return -1;
}

static int kfetch_record_runtime(void *args)
{
    pr_info("kfetch_record_runtime is running.\n");
    struct proc_timeinfo *timeinfos = NULL;
    u32 current_num = 0;
    u64 *boottimes = NULL;
    while (!kthread_should_stop())
    {
        // Read the number of tasks and realloc buffer spaces
        struct task_struct *task;
        u32 task_num = 0;
        rcu_read_lock();
        for_each_process(task)
        {
            task_num++;
        }
        rcu_read_unlock();
        u32 task_num_with_guard = task_num + task_num / 2;
        if (timeinfos == NULL || boottimes == NULL)
        {
            timeinfos = kzalloc(task_num_with_guard * sizeof(struct proc_timeinfo), GFP_KERNEL);
            boottimes = kzalloc(task_num_with_guard * sizeof(u64), GFP_KERNEL);
            if(timeinfos != NULL && boottimes != NULL)
            {
                current_num = task_num_with_guard;
            }
        }
        else
        {
            struct proc_timeinfo *temp_infos = krealloc(timeinfos, task_num_with_guard * sizeof(struct proc_timeinfo), GFP_KERNEL);
            u64 *temp_boottimes = krealloc(boottimes, task_num_with_guard * sizeof(u64), GFP_KERNEL);
            if(temp_infos != NULL)
            {
                timeinfos = temp_infos;
            }
            if(temp_boottimes != NULL)
            {
                boottimes = temp_boottimes;
            }
            if(temp_infos != NULL && temp_boottimes != NULL)
            {
                current_num = task_num_with_guard;
            }
        }

        if (timeinfos != NULL && boottimes != NULL)
        {
            // Store task informations in the buffers
            u32 info_num = 0;
            rcu_read_lock();
            for_each_process(task)
            {
                timeinfos[info_num].pid = task->pid;
                timeinfos[info_num].utime = task->utime;
                timeinfos[info_num].stime = task->stime;
                timeinfos[info_num].timestamp = ktime_get_ns();
                timeinfos[info_num].core_id = get_core_id(task_cpu(task));
                boottimes[info_num] = task->start_boottime;
                info_num++;
                if (info_num == current_num)
                {
                    break;
                }
            }
            rcu_read_unlock();

            for (int i = 0; i < info_num; i++)
            {
                update_timeinfo(timeinfos + i, boottimes[i]);
            }
        }

        if (kthread_should_stop())
        {
            break;
        }

        msleep_interruptible(record_rate_ms);
    }
    kfree(timeinfos);
    kfree(boottimes);
    return 0;
}

static int kfetch_calculate_power(void *args)
{
    pr_info("kfetch_calculate_power is running.\n");
    u64 pkg_sum = 0;
    u64 *copy = kzalloc(core_num * sizeof(u64), GFP_KERNEL);
    if (copy == NULL)
    {
        pr_info("Failed to allocate buffer spaces in kfetch_calculate_power.\n");
        return -1;
    }
    while (!kthread_should_stop())
    {
        // Record counters into buffers
        pkg_sum = 0;
        mutex_lock(&counter_lock);
        for (int i = 0; i < core_num; i++)
        {
            copy[i] = (cpu_online(i)) ? counters_core[i].curr : 0;
        }
        for (int i = 0; i < pkg_num; i++)
        {
            pkg_sum += counters_pkg[i].curr;
        }
        mutex_unlock(&counter_lock);

        // Calculate energy consumed by cores and packages
        mutex_lock(&joule_lock);
        for (int i = 0; i < core_num; i++)
        {
            if (cpu_online(i))
            {
                core_uj[i][0] = core_uj[i][1];
                core_uj[i][1] = div64_ul(copy[i] * 1000000UL, BIT(energy_unit));
            }
            else
            {
                core_uj[i][0] = 0;
                core_uj[i][1] = 0;
            }
        }
        pkg_uj[0] = pkg_uj[1];
        pkg_uj[1] = div64_ul(pkg_sum * 1000000UL, BIT(energy_unit));
        mutex_unlock(&joule_lock);

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
        for (int cpu = 0; cpu < core_num; cpu++)
        {
            if (cpu_online(cpu))
            {
                update_counter(counters_core + cpu, cpu, MSR_CORE_ENERGY_STATUS);
            }
        }
        // package
        for (int pkg = 0; pkg < pkg_num; pkg++)
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
             "%*c" TITLE_COLOR "%s" RESET_COLOR "\n", kfetch_logo_width, ' ', hostname);

    // separate line
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
    ssize_t ret = 0;
    u64 *core_mws = kzalloc(core_num * sizeof(u64), GFP_KERNEL);
    struct proc_powerinfo *powerinfos = kzalloc(timeinfo_num * sizeof(struct proc_powerinfo), GFP_KERNEL);
    if (core_mws == NULL || powerinfos == NULL)
    {
        pr_info("Failed to allocate buffer spaces when reading power info.\n");
        return strlen(buffer);
    }

    char buf[KFETCH_INFO_SIZE] = "";
    char separate_line[KFETCH_INFO_SIZE] = "";
    memset(separate_line, '=', KFETCH_INFO_SIZE - 2);
    separate_line[KFETCH_INFO_SIZE - 2] = '\n';
    separate_line[KFETCH_INFO_SIZE - 1] = '\0';
    strncat(buffer, separate_line, buffer_size - strlen(buffer) - 1);

    if ((mask >> 6) & 1)
    {
        mutex_lock(&joule_lock);
        u64 core_mw = 0;
        u64 pkg_mw = div64_ul(pkg_uj[1] - pkg_uj[0], calculate_rate_ms);
        for (int i = 0; i < core_num; i++)
        {
            if (cpu_online(i))
            {
                core_mws[i] = div64_ul(core_uj[i][1] - core_uj[i][0], calculate_rate_ms);
                core_mw += core_mws[i];
            }
        }
        mutex_unlock(&joule_lock);
        if (core_mw > __LONG_LONG_MAX__ || pkg_mw > __LONG_LONG_MAX__)
        {
            pr_info("Power of cpu cores or packages overflow\n");
            ret = -1;
            goto cleanup;
        }

        if (core_mw == 0 || pkg_mw == 0)
        {
            snprintf(buf, KFETCH_INFO_SIZE, "Power of cpu cores or packages == 0.\n");
        }
        else
        {
            snprintf(buf, KFETCH_INFO_SIZE, "pkg power: %llumW\tcore power: %llumW.\n", pkg_mw, core_mw);
        }

        strncat(buffer, buf, buffer_size - strlen(buffer) - 1);
    }
    if ((mask >> 7) & 1)
    {
        u32 powerinfo_num = 0;
        struct proc_timeinfo *timeinfo;
        struct hlist_node *tmp;
        int bucket;
        hash_for_each_safe(timeinfo_table, bucket, tmp, timeinfo, node)
        {
            struct task_struct *task = find_task(timeinfo->pid);
            mutex_lock(&timeinfo_lock);
            if (task == NULL)
            {
                hash_del(&timeinfo->node);
                kfree(timeinfo);
                timeinfo_num--;
            }
            else
            {
                powerinfos[powerinfo_num].pid = task->pid;
                powerinfos[powerinfo_num].name = task->comm;
                powerinfos[powerinfo_num].mw = core_mws[timeinfo->core_id] * timeinfo->cpu_ratio / 1000;
                powerinfo_num++;
            }
            mutex_unlock(&timeinfo_lock);
        }
        sort(powerinfos, powerinfo_num, sizeof(struct proc_powerinfo), compare_power, NULL);

        snprintf(buf, KFETCH_INFO_SIZE, "pid\tprocess name\twatt(mw)\n");
        strncat(buffer, buf, buffer_size - strlen(buffer) - 1);
        for (int i = 0; i < min(10, powerinfo_num) && powerinfos[i].mw; i++)
        {
            snprintf(buf, KFETCH_INFO_SIZE, "%d\t%-*s%llu\n", powerinfos[i].pid,
                     TASK_COMM_LEN, powerinfos[i].name, powerinfos[i].mw);
            strncat(buffer, buf, buffer_size - strlen(buffer) - 1);
        }
    }

    strncat(buffer, separate_line, buffer_size - strlen(buffer) - 1);
    ret = strlen(buffer);

cleanup:
    kfree(core_mws);
    kfree(powerinfos);

    return ret;
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