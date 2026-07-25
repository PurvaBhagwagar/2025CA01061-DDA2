// SPDX-License-Identifier: GPL-2.0
/*
 * bitsmon.c - kthread sampler with workqueue-based deferred statistics
 *
 * A kernel thread appends {seq, jiffies, ktime_ns} samples to a
 * spinlock-protected list every interval_ms milliseconds. A delayed
 * work item periodically drains the list onto a private list, computes
 * min/max/avg inter-sample gap, and stores the result under a mutex.
 * Userspace can read the latest snapshot from /dev/bitsmon and tune
 * interval_ms via /sys/class/bitsmon/bitsmon/interval_ms.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/ktime.h>
#include <linux/jiffies.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/wait.h>
#include <linux/sched.h>

#define DRV_NAME		"bitsmon"
#define DEFAULT_INTERVAL_MS	1000
#define MIN_INTERVAL_MS		100
#define MAX_INTERVAL_MS		5000
#define DRAIN_PERIOD_MS		5000
#define SNAPSHOT_BUF_SZ		256

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Purva Bhagwagar, 2025CA01061");
MODULE_DESCRIPTION("bitsmon: kthread sampler with deferred workqueue statistics");

/* ---- sample record produced by the kthread, protected by sample_lock ---- */
struct sample_record {
	struct list_head list;
	unsigned int seq;
	unsigned long jiffies_val;
	u64 ktime_ns;
};

static LIST_HEAD(sample_list);
static DEFINE_SPINLOCK(sample_lock);
static unsigned int seq_counter;

/* ---- computed statistics, protected by stats_lock ---- */
struct bitsmon_stats {
	unsigned int n;
	u64 min_gap_ns;
	u64 max_gap_ns;
	u64 avg_gap_ns;
	bool valid;
	bool have_prev;
	u64 prev_ns;		/* last sample's timestamp across drains, for
				 * gap continuity between work invocations */
};

static struct bitsmon_stats stats;
static DEFINE_MUTEX(stats_lock);

/* ---- kthread + wait queue used for prompt, interruptible sleeps ---- */
static struct task_struct *bitsmon_task;
static DECLARE_WAIT_QUEUE_HEAD(bitsmon_waitq);
static unsigned int interval_ms = DEFAULT_INTERVAL_MS;

/* ---- delayed work ---- */
static struct workqueue_struct *bitsmon_wq;
static void bitsmon_work_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(bitsmon_dwork, bitsmon_work_fn);

/* ---- char device plumbing ---- */
static dev_t bitsmon_devno;
static struct cdev bitsmon_cdev;
static struct class *bitsmon_class;
static struct device *bitsmon_device;

/* ------------------------------------------------------------------ */

static int bitsmon_thread_fn(void *data)
{
	while (!kthread_should_stop()) {
		struct sample_record *rec;

		rec = kmalloc(sizeof(*rec), GFP_KERNEL);
		if (rec) {
			unsigned long flags;

			rec->seq = seq_counter++;
			rec->jiffies_val = jiffies;
			rec->ktime_ns = ktime_get_ns();

			spin_lock_irqsave(&sample_lock, flags);
			list_add_tail(&rec->list, &sample_list);
			spin_unlock_irqrestore(&sample_lock, flags);
		} else {
			pr_warn(DRV_NAME ": sample allocation failed, skipping\n");
		}

		/*
		 * wait_event_interruptible_timeout sleeps in TASK_INTERRUPTIBLE.
		 * kthread_stop() calls wake_up_process() on this task directly,
		 * which wakes it out of this sleep immediately regardless of
		 * remaining timeout, so shutdown is prompt even mid-interval.
		 */
		wait_event_interruptible_timeout(bitsmon_waitq,
						  kthread_should_stop(),
						  msecs_to_jiffies(READ_ONCE(interval_ms)));
	}

	return 0;
}

/* Drains sample_list onto a private list under the spinlock, then
 * processes it lock-free. This is why the drain can't run directly
 * under sample_lock: computing min/max/avg and calling pr_info() are
 * not bounded, sleep-free operations, and holding a spinlock across
 * them would raise IRQ-disabled latency and risk deadlock if pr_info's
 * console path ever blocks. Moving to a private list keeps the
 * spinlock critical section O(1) (a single list_splice_init).
 */
static void bitsmon_work_fn(struct work_struct *work)
{
	LIST_HEAD(local_list);
	struct sample_record *rec, *tmp;
	unsigned long flags;
	u64 sum_ns = 0;
	u64 min_ns = U64_MAX;
	u64 max_ns = 0;
	unsigned int cnt = 0;
	u64 prev_ns;
	bool have_prev;

	spin_lock_irqsave(&sample_lock, flags);
	list_splice_init(&sample_list, &local_list);
	spin_unlock_irqrestore(&sample_lock, flags);

	mutex_lock(&stats_lock);
	prev_ns = stats.prev_ns;
	have_prev = stats.have_prev;
	mutex_unlock(&stats_lock);

	list_for_each_entry(rec, &local_list, list) {
		if (have_prev) {
			u64 gap = rec->ktime_ns - prev_ns;

			sum_ns += gap;
			if (gap < min_ns)
				min_ns = gap;
			if (gap > max_ns)
				max_ns = gap;
			cnt++;
		}
		prev_ns = rec->ktime_ns;
		have_prev = true;
	}

	mutex_lock(&stats_lock);
	stats.prev_ns = prev_ns;
	stats.have_prev = have_prev;
	if (cnt) {
		stats.n = cnt;
		stats.min_gap_ns = min_ns;
		stats.max_gap_ns = max_ns;
		stats.avg_gap_ns = div_u64(sum_ns, cnt);
		stats.valid = true;
		pr_info(DRV_NAME ": n=%u min=%llums max=%llums avg=%llums\n",
			stats.n,
			div_u64(stats.min_gap_ns, NSEC_PER_MSEC),
			div_u64(stats.max_gap_ns, NSEC_PER_MSEC),
			div_u64(stats.avg_gap_ns, NSEC_PER_MSEC));
	}
	mutex_unlock(&stats_lock);

	list_for_each_entry_safe(rec, tmp, &local_list, list) {
		list_del(&rec->list);
		kfree(rec);
	}

	queue_delayed_work(bitsmon_wq, &bitsmon_dwork,
			    msecs_to_jiffies(DRAIN_PERIOD_MS));
}

/* ---- /dev/bitsmon fops ---- */

static ssize_t bitsmon_read(struct file *filp, char __user *ubuf,
			     size_t count, loff_t *ppos)
{
	char kbuf[SNAPSHOT_BUF_SZ];
	int len;

	mutex_lock(&stats_lock);
	if (stats.valid) {
		len = scnprintf(kbuf, sizeof(kbuf),
				 "n=%u min_ms=%llu max_ms=%llu avg_ms=%llu\n",
				 stats.n,
				 div_u64(stats.min_gap_ns, NSEC_PER_MSEC),
				 div_u64(stats.max_gap_ns, NSEC_PER_MSEC),
				 div_u64(stats.avg_gap_ns, NSEC_PER_MSEC));
	} else {
		len = scnprintf(kbuf, sizeof(kbuf), "no stats yet\n");
	}
	mutex_unlock(&stats_lock);

	return simple_read_from_buffer(ubuf, count, ppos, kbuf, len);
}

static const struct file_operations bitsmon_fops = {
	.owner = THIS_MODULE,
	.read  = bitsmon_read,
};

/* ---- sysfs: interval_ms (rw, clamped 100-5000) ---- */

static ssize_t interval_ms_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(interval_ms));
}

static ssize_t interval_ms_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 10, &val);
	if (ret)
		return ret;

	val = clamp_val(val, MIN_INTERVAL_MS, MAX_INTERVAL_MS);
	WRITE_ONCE(interval_ms, val);

	return count;
}

static DEVICE_ATTR_RW(interval_ms);

/* ---- module init/exit ---- */

static int __init bitsmon_init(void)
{
	int ret;

	memset(&stats, 0, sizeof(stats));

	bitsmon_wq = alloc_ordered_workqueue(DRV_NAME, 0);
	if (!bitsmon_wq)
		return -ENOMEM;

	ret = alloc_chrdev_region(&bitsmon_devno, 0, 1, DRV_NAME);
	if (ret)
		goto err_wq;

	cdev_init(&bitsmon_cdev, &bitsmon_fops);
	bitsmon_cdev.owner = THIS_MODULE;
	ret = cdev_add(&bitsmon_cdev, bitsmon_devno, 1);
	if (ret)
		goto err_chrdev;

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
	bitsmon_class = class_create(THIS_MODULE, DRV_NAME);
#else
	bitsmon_class = class_create(DRV_NAME);
#endif
	if (IS_ERR(bitsmon_class)) {
		ret = PTR_ERR(bitsmon_class);
		goto err_cdev;
	}

	bitsmon_device = device_create(bitsmon_class, NULL, bitsmon_devno,
					NULL, DRV_NAME);
	if (IS_ERR(bitsmon_device)) {
		ret = PTR_ERR(bitsmon_device);
		goto err_class;
	}

	ret = device_create_file(bitsmon_device, &dev_attr_interval_ms);
	if (ret)
		goto err_device;

	bitsmon_task = kthread_run(bitsmon_thread_fn, NULL, "bitsmon-worker");
	if (IS_ERR(bitsmon_task)) {
		ret = PTR_ERR(bitsmon_task);
		goto err_attr;
	}

	queue_delayed_work(bitsmon_wq, &bitsmon_dwork,
			    msecs_to_jiffies(DRAIN_PERIOD_MS));

	pr_info(DRV_NAME ": loaded, major=%d minor=%d interval_ms=%u\n",
		MAJOR(bitsmon_devno), MINOR(bitsmon_devno), interval_ms);
	return 0;

err_attr:
	device_remove_file(bitsmon_device, &dev_attr_interval_ms);
err_device:
	device_destroy(bitsmon_class, bitsmon_devno);
err_class:
	class_destroy(bitsmon_class);
err_cdev:
	cdev_del(&bitsmon_cdev);
err_chrdev:
	unregister_chrdev_region(bitsmon_devno, 1);
err_wq:
	destroy_workqueue(bitsmon_wq);
	return ret;
}

static void __exit bitsmon_exit(void)
{
	struct sample_record *rec, *tmp;
	unsigned long flags;

	kthread_stop(bitsmon_task);
	cancel_delayed_work_sync(&bitsmon_dwork);
	destroy_workqueue(bitsmon_wq);

	/* free every remaining record so unload never leaks */
	spin_lock_irqsave(&sample_lock, flags);
	list_for_each_entry_safe(rec, tmp, &sample_list, list) {
		list_del(&rec->list);
		kfree(rec);
	}
	spin_unlock_irqrestore(&sample_lock, flags);

	device_remove_file(bitsmon_device, &dev_attr_interval_ms);
	device_destroy(bitsmon_class, bitsmon_devno);
	class_destroy(bitsmon_class);
	cdev_del(&bitsmon_cdev);
	unregister_chrdev_region(bitsmon_devno, 1);

	pr_info(DRV_NAME ": unloaded\n");
}

module_init(bitsmon_init);
module_exit(bitsmon_exit);
