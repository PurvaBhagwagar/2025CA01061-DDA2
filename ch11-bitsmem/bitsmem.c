// SPDX-License-Identifier: GPL-2.0
/*
 * bitsmem.c - private slab cache playground + kmalloc vs vmalloc benchmark
 *
 * A private kmem_cache holds fixed-size 64-byte records. Userspace
 * drives allocation/free through text commands written to /dev/bitsmem
 * ("alloc N", "free N", "bench"); read() returns a status report.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/ktime.h>
#include <linux/string.h>

#define DRV_NAME	"bitsmem"
#define CMD_BUF_SZ	64
#define REPORT_BUF_SZ	512
#define BENCH_ITERS	100

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Purva Bhagwagar, 2025CA01061");
MODULE_DESCRIPTION("bitsmem: slab cache playground and kmalloc/vmalloc benchmark");

/* Deliberately sized to 64 bytes on a 64-bit build:
 * list_head (16 bytes) + 6 x u64 payload (48 bytes) = 64 bytes.
 */
struct bits_rec {
	struct list_head list;
	u64 payload[6];
};

static struct kmem_cache *bits_cache;
/*
 * A non-NULL constructor disqualifies this cache from SLUB's cache-merging
 * logic (find_mergeable() bails out whenever ctor is set), so "bits_rec"
 * always shows up under its own name in /proc/slabinfo instead of silently
 * being folded into an existing same-sized cache like kmalloc-64.
 */
static void bits_rec_ctor(void *obj)
{
	/* intentionally empty */
}

static LIST_HEAD(allocated_list);
static DEFINE_MUTEX(bitsmem_lock);
static unsigned int outstanding;
static unsigned int high_water_mark;

struct bench_result {
	u64 kmalloc_64k_ns;
	u64 vmalloc_64k_ns;
	u64 kmalloc_1m_ns;
	u64 vmalloc_1m_ns;
	bool valid;
};

static struct bench_result last_bench;

/* ------------------------------------------------------------------ */

static int bitsmem_alloc(unsigned int n)
{
	unsigned int i;
	LIST_HEAD(batch);

	for (i = 0; i < n; i++) {
		struct bits_rec *rec = kmem_cache_alloc(bits_cache, GFP_KERNEL);

		if (!rec) {
			struct bits_rec *r, *tmp;

			/* free only this partial batch, leave prior state intact */
			list_for_each_entry_safe(r, tmp, &batch, list) {
				list_del(&r->list);
				kmem_cache_free(bits_cache, r);
			}
			return -ENOMEM;
		}
		list_add_tail(&rec->list, &batch);
	}

	mutex_lock(&bitsmem_lock);
	list_splice_tail(&batch, &allocated_list);
	outstanding += n;
	if (outstanding > high_water_mark)
		high_water_mark = outstanding;
	mutex_unlock(&bitsmem_lock);

	return 0;
}

static int bitsmem_free(unsigned int n)
{
	struct bits_rec *rec, *tmp;
	unsigned int freed = 0;

	mutex_lock(&bitsmem_lock);
	list_for_each_entry_safe(rec, tmp, &allocated_list, list) {
		if (freed >= n)
			break;
		list_del(&rec->list);
		kmem_cache_free(bits_cache, rec);
		freed++;
	}
	outstanding -= freed;
	mutex_unlock(&bitsmem_lock);

	return 0;
}

static u64 bench_kmalloc(size_t size)
{
	int i;
	ktime_t start = ktime_get();

	for (i = 0; i < BENCH_ITERS; i++) {
		void *p = kmalloc(size, GFP_KERNEL);

		if (p)
			kfree(p);
	}
	return div_u64(ktime_to_ns(ktime_sub(ktime_get(), start)), BENCH_ITERS);
}

static u64 bench_vmalloc(size_t size)
{
	int i;
	ktime_t start = ktime_get();

	for (i = 0; i < BENCH_ITERS; i++) {
		void *p = vmalloc(size);

		if (p)
			vfree(p);
	}
	return div_u64(ktime_to_ns(ktime_sub(ktime_get(), start)), BENCH_ITERS);
}

static void bitsmem_bench(void)
{
	struct bench_result r;

	r.kmalloc_64k_ns = bench_kmalloc(64 * 1024);
	r.vmalloc_64k_ns = bench_vmalloc(64 * 1024);
	r.kmalloc_1m_ns  = bench_kmalloc(1024 * 1024);
	r.vmalloc_1m_ns  = bench_vmalloc(1024 * 1024);
	r.valid = true;

	mutex_lock(&bitsmem_lock);
	last_bench = r;
	mutex_unlock(&bitsmem_lock);

	pr_info(DRV_NAME ": bench 64K kmalloc=%lluns vmalloc=%lluns | 1M kmalloc=%lluns vmalloc=%lluns\n",
		r.kmalloc_64k_ns, r.vmalloc_64k_ns, r.kmalloc_1m_ns, r.vmalloc_1m_ns);
}

/* ---- /dev/bitsmem fops ---- */

static ssize_t bitsmem_write(struct file *filp, const char __user *ubuf,
			      size_t count, loff_t *ppos)
{
	char kbuf[CMD_BUF_SZ];
	char cmd[16];
	unsigned int n = 0;
	int ret;

	if (count == 0 || count >= sizeof(kbuf))
		return -EINVAL;

	if (copy_from_user(kbuf, ubuf, count))
		return -EFAULT;
	kbuf[count] = '\0';

	/* strip trailing newline left by `echo`/`tee` */
	if (count && kbuf[count - 1] == '\n')
		kbuf[count - 1] = '\0';

	if (sscanf(kbuf, "%15s %u", cmd, &n) == 2 && !strcmp(cmd, "alloc")) {
		ret = bitsmem_alloc(n);
		if (ret)
			return ret;
	} else if (sscanf(kbuf, "%15s %u", cmd, &n) == 2 && !strcmp(cmd, "free")) {
		ret = bitsmem_free(n);
		if (ret)
			return ret;
	} else if (sscanf(kbuf, "%15s", cmd) == 1 && !strcmp(cmd, "bench")) {
		bitsmem_bench();
	} else {
		return -EINVAL;
	}

	return count;
}

static ssize_t bitsmem_read(struct file *filp, char __user *ubuf,
			     size_t count, loff_t *ppos)
{
	char kbuf[REPORT_BUF_SZ];
	int len;
	unsigned int out, hwm;
	struct bench_result b;

	mutex_lock(&bitsmem_lock);
	out = outstanding;
	hwm = high_water_mark;
	b = last_bench;
	mutex_unlock(&bitsmem_lock);

	len = scnprintf(kbuf, sizeof(kbuf),
			 "outstanding=%u hwm=%u obj_size=%u\n",
			 out, hwm, kmem_cache_size(bits_cache));

	if (b.valid)
		len += scnprintf(kbuf + len, sizeof(kbuf) - len,
				  "bench_ns: kmalloc_64k=%llu vmalloc_64k=%llu kmalloc_1m=%llu vmalloc_1m=%llu\n",
				  b.kmalloc_64k_ns, b.vmalloc_64k_ns,
				  b.kmalloc_1m_ns, b.vmalloc_1m_ns);
	else
		len += scnprintf(kbuf + len, sizeof(kbuf) - len,
				  "bench_ns: not run yet\n");

	return simple_read_from_buffer(ubuf, count, ppos, kbuf, len);
}

static const struct file_operations bitsmem_fops = {
	.owner = THIS_MODULE,
	.read  = bitsmem_read,
	.write = bitsmem_write,
};

static struct miscdevice bitsmem_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = DRV_NAME,
	.fops  = &bitsmem_fops,
	.mode  = 0666,
};

/* ---- module init/exit ---- */

static int __init bitsmem_init(void)
{
	int ret;

	bits_cache = kmem_cache_create("bits_rec", sizeof(struct bits_rec),
					0, SLAB_HWCACHE_ALIGN, bits_rec_ctor);
	if (!bits_cache)
		return -ENOMEM;

	ret = misc_register(&bitsmem_miscdev);
	if (ret) {
		kmem_cache_destroy(bits_cache);
		return ret;
	}

	pr_info(DRV_NAME ": loaded, object size=%u\n",
		kmem_cache_size(bits_cache));
	return 0;
}

static void __exit bitsmem_exit(void)
{
	struct bits_rec *rec, *tmp;

	misc_deregister(&bitsmem_miscdev);

	/* free every outstanding record before destroying the cache */
	mutex_lock(&bitsmem_lock);
	list_for_each_entry_safe(rec, tmp, &allocated_list, list) {
		list_del(&rec->list);
		kmem_cache_free(bits_cache, rec);
	}
	outstanding = 0;
	mutex_unlock(&bitsmem_lock);

	kmem_cache_destroy(bits_cache);
	pr_info(DRV_NAME ": unloaded\n");
}

module_init(bitsmem_init);
module_exit(bitsmem_exit);
