// SPDX-License-Identifier: GPL-2.0
/*
 * mi_reclaim daemon.
 *
 * mi_reclaim_global() lives in mm/vmscan.c because it needs struct
 * scan_control and the static do_try_to_free_pages().
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/sysinfo.h>
#include <linux/sched.h>
#include <linux/freezer.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/ktime.h>
#include <linux/vmstat.h>
#include <linux/cpumask.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/string.h>

#include <linux/mi_reclaim.h>

struct pg_mi_reclaim {
	u8 enable;			/* 0x00 */
	u8 debug;			/* 0x01 */
	u8 event_flag;			/* 0x02 */
	u8 pad0[5];			/* 0x03 */
	wait_queue_head_t wait;		/* 0x08 */
	struct kobject *kobj;		/* 0x20 */
	struct task_struct *thread;	/* 0x28 */
	int swappiness;			/* 0x30, default 120 */
	int reclaim_type;		/* 0x34, default 0 */
	int event;			/* 0x38, default 0 */
	u32 pad1;			/* 0x3c */
	u64 last_time;			/* 0x40 */
	u64 once_reclaim_time_up;	/* 0x48, default 5 s in ns */
	u64 nr_reclaim;			/* 0x50, default 0x1e00 pages */
	u64 anon_up;			/* 0x58, default 0x4cccc pages */
	u64 file_up;			/* 0x60, default 0x66666 pages */
	u64 total_spent_time;		/* 0x68 */
	u64 total_reclaim_pages;	/* 0x70 */
	u64 total_reclaim_times;	/* 0x78 */
	spinlock_t lock;		/* 0x80 */
	u64 totalram;			/* 0x88 */
};

static struct pg_mi_reclaim g_mi_reclaim;
static struct pg_mi_reclaim *pg_mi_reclaim;

/* re-armed at the end of every pass */
#define MI_RECLAIM_DEFAULT_NR	0x1e00UL
#define MI_RECLAIM_EVENT2_NR	0x9600UL
#define MI_RECLAIM_AUTO_NR	0x6400UL
#define MI_RECLAIM_CHUNK	32UL
#define MI_RECLAIM_MAX_LOW	512
#define MI_RECLAIM_AUTO_MIN_CACHE	0x4ccccUL
#define MI_RECLAIM_SWAP_MIN	125	/* (si.freeswap >> 10) */

bool mi_reclaim(const char *name)
{
	return strncmp("mi_reclaim", name, strlen("mi_reclaim")) == 0;
}

bool mi_st(void)
{
	return pg_mi_reclaim->event == 2;
}

int mi_reclaim_swappiness(void)
{
	return pg_mi_reclaim->swappiness;
}

static void mi_reclaim_debug_log(struct sysinfo *si, int type,
				 unsigned long reclaimed, long inactive_anon,
				 long active_anon, int low_count)
{
	si_meminfo(si);
	si_swapinfo(si);

	pr_info("reclaim: reclaim_type=%d real_reclaim=%lu freeram=%llu freeswap=%llu MemAvailable=%ld nr_inactive_anon_pages=%lu nr_active_anon_pages=%lu reclaim_ratio_contiguous_low_count %d",
		type, reclaimed,
		(u64)si->freeram << 2, (u64)si->freeswap << 2,
		si_mem_available() << 2,
		(unsigned long)inactive_anon, (unsigned long)active_anon,
		low_count);
}

static u64 mi_reclaim_spent_time(ktime_t start)
{
	s64 spent = ktime_to_ns(ktime_sub(ktime_get(), start));

	/* never report a zero delta */
	if (!spent)
		spent = 1;
	return spent;
}

static int mi_reclaim_thread(void *data)
{
	struct pg_mi_reclaim *pg = pg_mi_reclaim;
	struct sysinfo si;
	ktime_t start;
	unsigned long nr_cached;
	unsigned long nr_reclaim, anon_up, file_up, once_reclaim_time_up;
	unsigned long reclaimed, total_reclaimed, total_spent;
	unsigned long window;
	long inactive_anon, active_anon, anon_free;
	int reclaim_type, event, page_type, low_count;

	(void)data;

	/* allocations from this thread may use memory reserves */
	current->flags |= PF_MEMALLOC;
	set_freezable();

	while (!kthread_should_stop()) {
		cpumask_t mask;

		try_to_freeze();

		/* keep the worker on the first four CPUs when possible */
		cpumask_clear(&mask);
		cpumask_set_cpu(0, &mask);
		cpumask_set_cpu(1, &mask);
		cpumask_set_cpu(2, &mask);
		cpumask_set_cpu(3, &mask);
		if (!cpumask_equal(&current->cpus_allowed, &mask) &&
		    cpumask_intersects(&mask, cpu_online_mask)) {
			if (sched_setaffinity(current->pid, &mask))
				pr_err("mi_reclaim sched_setaffinity() failed");
		}

		wait_event_freezable(pg->wait,
				     pg->event_flag || kthread_should_stop());

		si_meminfo(&si);
		si_swapinfo(&si);

		nr_cached = 0;
		total_reclaimed = 0;
		total_spent = 0;
		event = 0;
		nr_reclaim = 0;
		anon_up = 0;
		file_up = 0;
		once_reclaim_time_up = 0;
		reclaim_type = 0;

		/*
		 * Auto trigger gate: only do anything when there is swap and
		 * at least 125 KiB of it is free.
		 */
		if (si.totalswap && (si.freeswap >> 10) >= MI_RECLAIM_SWAP_MIN) {
			nr_cached = atomic_long_read(&vm_node_stat[NR_FILE_PAGES]);
			if ((long)nr_cached < 0)
				nr_cached = 0;
			nr_cached -= total_swapcache_pages() + si.bufferram;

			if ((long)nr_cached < (long)MI_RECLAIM_AUTO_MIN_CACHE)
				goto update;

			spin_lock(&pg->lock);
			anon_up = pg->anon_up;
			file_up = pg->file_up;
			reclaim_type = pg->reclaim_type;
			event = pg->event;
			once_reclaim_time_up = pg->once_reclaim_time_up;
			nr_reclaim = pg->nr_reclaim;
			spin_unlock(&pg->lock);

			if (event == 2) {
				/* explicit "2": immediate refill pass */
				pg->last_time = jiffies;
				if (!nr_reclaim)
					goto update;
			} else {
				/* automatic pass: at most once every 3 s */
				if (jiffies_to_msecs(jiffies - (unsigned long)pg->last_time) < 3000)
					goto update;
				if (nr_cached < file_up) {
					event = 3;
					nr_reclaim = MI_RECLAIM_AUTO_NR;
				}
				pg->last_time = jiffies;
				if (!nr_reclaim)
					goto update;
			}

			page_type = reclaim_type ? reclaim_type : 3;

			if (reclaim_type) {
				/*
				 * Variant A: reclaim_type was set explicitly,
				 * always issue at least one chunk.
				 */
				window = MI_RECLAIM_CHUNK;
				low_count = 0;
				for (;;) {
					start = ktime_get();
					inactive_anon = atomic_long_read(&vm_node_stat[NR_INACTIVE_ANON]);
					active_anon = atomic_long_read(&vm_node_stat[NR_ACTIVE_ANON]);

					reclaimed = mi_reclaim_global(MI_RECLAIM_CHUNK,
								      page_type);
					if (reclaimed > 9) {
						low_count = 0;
					} else if (++low_count > MI_RECLAIM_MAX_LOW) {
						break;
					}

					if (inactive_anon < 0)
						inactive_anon = 0;
					if (active_anon < 0)
						active_anon = 0;

					if (pg->debug)
						mi_reclaim_debug_log(&si, page_type,
								     reclaimed,
								     inactive_anon,
								     active_anon,
								     low_count);

					anon_free = inactive_anon + active_anon;
					total_reclaimed += reclaimed;
					total_spent += mi_reclaim_spent_time(start);

					if (anon_free < anon_up)
						break;
					if (total_reclaimed > nr_reclaim)
						break;
					if (window >= nr_reclaim)
						break;
					if (!((event | 1) == 3 ||
					      total_spent <= once_reclaim_time_up))
						break;
					window += MI_RECLAIM_CHUNK;
				}
			} else {
				/*
				 * Variant B: automatic mode.  Only reclaim
				 * anonymous pages if enough of them are free.
				 */
				window = MI_RECLAIM_CHUNK;
				low_count = 0;
				for (;;) {
					int dbg_type = page_type;

					start = ktime_get();
					inactive_anon = atomic_long_read(&vm_node_stat[NR_INACTIVE_ANON]);
					active_anon = atomic_long_read(&vm_node_stat[NR_ACTIVE_ANON]);
					if (inactive_anon < 0)
						inactive_anon = 0;
					if (active_anon < 0)
						active_anon = 0;
					anon_free = inactive_anon + active_anon;

					if (anon_free > anon_up) {
						reclaimed = mi_reclaim_global(MI_RECLAIM_CHUNK,
									      page_type);
						if (reclaimed > 9) {
							low_count = 0;
						} else if (++low_count > MI_RECLAIM_MAX_LOW) {
							break;
						}
					} else {
						reclaimed = 0;
						dbg_type = 0;
					}

					if (pg->debug)
						mi_reclaim_debug_log(&si, dbg_type,
								     reclaimed,
								     inactive_anon,
								     active_anon,
								     low_count);

					total_reclaimed += reclaimed;
					total_spent += mi_reclaim_spent_time(start);

					if (anon_free < anon_up)
						break;
					if (total_reclaimed > nr_reclaim)
						break;
					if (window >= nr_reclaim)
						break;
					if (!((event | 1) == 3 ||
					      total_spent <= once_reclaim_time_up))
						break;
					window += MI_RECLAIM_CHUNK;
				}
			}
		}

update:
		spin_lock(&pg->lock);
		pg->event_flag = 0;
		pg->total_reclaim_pages += total_reclaimed;
		pg->total_reclaim_times++;
		pg->total_spent_time += total_spent;
		pg->nr_reclaim = MI_RECLAIM_DEFAULT_NR;
		spin_unlock(&pg->lock);

		if (pg->debug)
			pr_info("reclaim event_type %d nr_cached %ld spent_time %llu ns nr_want_reclaim_pages %lu reclaim_pages %llu once_reclaim_time_up %lu\n",
				event, (long)nr_cached,
				(unsigned long long)total_spent,
				nr_reclaim,
				(unsigned long long)total_reclaimed,
				once_reclaim_time_up);
	}

	current->flags &= ~PF_MEMALLOC;
	return 0;
}

/* sysfs interface: /sys/kernel/mi_reclaim/ */

static ssize_t enable_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", pg_mi_reclaim->enable);
}

static ssize_t enable_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	int val, ret;

	ret = kstrtoint(buf, 0, &val);
	if (ret)
		return ret;

	pg_mi_reclaim->enable = val != 0;
	return count;
}

static ssize_t event_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", pg_mi_reclaim->event);
}

static ssize_t event_store(struct kobject *kobj, struct kobj_attribute *attr,
			   const char *buf, size_t count)
{
	unsigned long long val;
	int ret;

	ret = kstrtoull(buf, 0, &val);
	if (ret)
		return ret;

	if (val == 0) {
		spin_lock(&pg_mi_reclaim->lock);
		pg_mi_reclaim->event = 0;
		pg_mi_reclaim->nr_reclaim = MI_RECLAIM_DEFAULT_NR;
		spin_unlock(&pg_mi_reclaim->lock);
	} else if (val == 1) {
		if (pg_mi_reclaim->enable && pg_mi_reclaim->thread &&
		    pg_mi_reclaim->thread->state) {
			pg_mi_reclaim->event_flag = 1;
			__wake_up(&pg_mi_reclaim->wait, TASK_NORMAL, 1, NULL);
		}
	} else if (val == 2) {
		spin_lock(&pg_mi_reclaim->lock);
		pg_mi_reclaim->nr_reclaim = MI_RECLAIM_EVENT2_NR;
		pg_mi_reclaim->event = 2;
		spin_unlock(&pg_mi_reclaim->lock);
	} else if (((val >> 1) & 0x1fffffffffffffULL) >= 15) {
		spin_lock(&pg_mi_reclaim->lock);
		pg_mi_reclaim->nr_reclaim = (val << 8) & 0x3fffffffffffffffULL;
		spin_unlock(&pg_mi_reclaim->lock);

		if (pg_mi_reclaim->enable && pg_mi_reclaim->thread &&
		    pg_mi_reclaim->thread->state) {
			pg_mi_reclaim->event_flag = 1;
			__wake_up(&pg_mi_reclaim->wait, TASK_NORMAL, 1, NULL);
		}
	}

	return count;
}

static ssize_t debug_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", pg_mi_reclaim->debug);
}

static ssize_t debug_store(struct kobject *kobj, struct kobj_attribute *attr,
			   const char *buf, size_t count)
{
	int val, ret;

	ret = kstrtoint(buf, 0, &val);
	if (ret)
		return ret;

	pg_mi_reclaim->debug = val != 0;
	return count;
}

static ssize_t config_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	int swappiness, reclaim_type;
	unsigned long nr_reclaim, anon_up, file_up, once_reclaim_time_up;
	u64 totalram;

	spin_lock(&pg_mi_reclaim->lock);
	swappiness = pg_mi_reclaim->swappiness;
	reclaim_type = pg_mi_reclaim->reclaim_type;
	nr_reclaim = pg_mi_reclaim->nr_reclaim;
	anon_up = pg_mi_reclaim->anon_up;
	file_up = pg_mi_reclaim->file_up;
	once_reclaim_time_up = pg_mi_reclaim->once_reclaim_time_up;
	totalram = pg_mi_reclaim->totalram;
	spin_unlock(&pg_mi_reclaim->lock);

	return snprintf(buf, PAGE_SIZE,
		"swappiness %d\nreclaim_type %d\nnr_reclaim %lu\nanon_up %lu\nfile_up %lu\nonce_reclaim_time_up %lu\ntotalram %llu\n",
		swappiness, reclaim_type, nr_reclaim, anon_up, file_up,
		once_reclaim_time_up, (unsigned long long)totalram);
}

static ssize_t config_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	int swappiness, reclaim_type, ret;
	unsigned long nr_reclaim, anon_up, file_up, once_reclaim_time_up;

	ret = sscanf(buf, "%d %d %lu %lu %lu %lu",
		     &swappiness, &reclaim_type, &nr_reclaim, &anon_up,
		     &file_up, &once_reclaim_time_up);
	if (ret != 6)
		return -EINVAL;

	spin_lock(&pg_mi_reclaim->lock);
	pg_mi_reclaim->swappiness = swappiness;
	pg_mi_reclaim->reclaim_type = reclaim_type;
	pg_mi_reclaim->nr_reclaim = nr_reclaim;
	pg_mi_reclaim->anon_up = anon_up;
	pg_mi_reclaim->file_up = file_up;
	pg_mi_reclaim->once_reclaim_time_up = once_reclaim_time_up;
	spin_unlock(&pg_mi_reclaim->lock);

	return count;
}

static ssize_t stat_show(struct kobject *kobj, struct kobj_attribute *attr,
			 char *buf)
{
	u64 total_reclaim_pages, total_reclaim_times, total_spent_time;

	spin_lock(&pg_mi_reclaim->lock);
	total_reclaim_pages = pg_mi_reclaim->total_reclaim_pages;
	total_reclaim_times = pg_mi_reclaim->total_reclaim_times;
	total_spent_time = pg_mi_reclaim->total_spent_time;
	spin_unlock(&pg_mi_reclaim->lock);

	return scnprintf(buf, PAGE_SIZE,
		"total_reclaim_pages: %llu\ntotal_reclaim_times: %llu\ntotal_spent_time: %llu\n",
		(unsigned long long)total_reclaim_pages,
		(unsigned long long)total_reclaim_times,
		(unsigned long long)total_spent_time);
}

static struct kobj_attribute kobj_attr_enable =
	__ATTR(enable, 0660, enable_show, enable_store);
static struct kobj_attribute kobj_attr_event =
	__ATTR(event, 0660, event_show, event_store);
static struct kobj_attribute kobj_attr_debug =
	__ATTR(debug, 0660, debug_show, debug_store);
static struct kobj_attribute kobj_attr_config =
	__ATTR(config, 0660, config_show, config_store);
static struct kobj_attribute kobj_attr_stat =
	__ATTR(stat, 0440, stat_show, NULL);

static struct attribute *mi_reclaim_attrs[] = {
	&kobj_attr_enable.attr,
	&kobj_attr_event.attr,
	&kobj_attr_debug.attr,
	&kobj_attr_config.attr,
	&kobj_attr_stat.attr,
	NULL,
};

static struct attribute_group mi_reclaim_attr_group = {
	.attrs = mi_reclaim_attrs,
};

static int __init mi_reclaim_init(void)
{
	struct pg_mi_reclaim *pg = &g_mi_reclaim;
	struct sysinfo si;
	struct task_struct *thread;
	struct kobject *kobj;

	pg_mi_reclaim = pg;
	si_meminfo(&si);

	pg->swappiness = 120;
	pg->reclaim_type = 0;
	pg->event = 0;
	pg->nr_reclaim = MI_RECLAIM_DEFAULT_NR;
	pg->anon_up = 0x4cccc;
	pg->file_up = 0x66666;
	pg->last_time = jiffies;
	pg->once_reclaim_time_up = 5000000000ULL;
	pg->totalram = si.totalram;

	init_waitqueue_head(&pg->wait);
	spin_lock_init(&pg->lock);

	kobj = kobject_create_and_add("mi_reclaim", kernel_kobj);
	if (!kobj) {
		pr_err("failed to create mi reclaim node.\n");
		pg->kobj = NULL;
		return -1;
	}

	if (sysfs_create_group(kobj, &mi_reclaim_attr_group)) {
		pr_err("failed to create mi reclaim attrs.\n");
		kobject_put(kobj);
		pg->kobj = NULL;
		return -1;
	}

	pg->kobj = kobj;
	if (pg->thread)
		return -1;

	thread = kthread_create_on_node(mi_reclaim_thread, NULL, -1,
					"mi_reclaim");
	if (IS_ERR(thread)) {
		pg->thread = thread;
		pr_err("failed to start pg_mi_reclaim thread\n");
		return -1;
	}

	pg->thread = thread;
	wake_up_process(thread);
	pg->enable = 1;
	pr_info("mi_reclaim init ok\n");

	return 0;
}

static void __exit mi_reclaim_exit(void)
{
	struct pg_mi_reclaim *pg = pg_mi_reclaim;
	struct task_struct *thread;

	if (pg->kobj)
		kobject_put(pg->kobj);

	thread = pg->thread;
	pg->kobj = NULL;

	if (thread) {
		if (pg->enable && thread->state) {
			pg->event_flag = 1;
			__wake_up(&pg->wait, TASK_NORMAL, 1, NULL);
		}
		kthread_stop(thread);
		pg->thread = NULL;
	}
}

module_init(mi_reclaim_init);
module_exit(mi_reclaim_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("mi_reclaim daemon");
