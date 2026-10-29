#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/fs.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/notifier.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/topology.h>
#include <linux/version.h>
#include <linux/workqueue.h>

#include "migt.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0)
#define cpus_read_lock()	get_online_cpus()
#define cpus_read_unlock()	put_online_cpus()
#endif

extern int mi_vip_task_req_get(void) __weak;

static unsigned int high_resolution_enable = 1;
static unsigned int migt_ms = 50;
static unsigned int migt_thresh = 18;

unsigned int minor_window_app;
unsigned int mi_viptask[3];

static unsigned int migt_debug;
static unsigned int tsched_debug;

static DECLARE_BITMAP(cluster_affinity_uid_mlist, MIGT_UID_MAX);
static DECLARE_BITMAP(cluster_affinity_uid_llist, MIGT_UID_MAX);
static DECLARE_BITMAP(cluster_affinity_uid_blist, MIGT_UID_MAX);

static bool migt_enabled;

static struct hrtimer hrtimer;
static struct workqueue_struct *migt_wq;
static struct delayed_work migt_work;
static struct work_struct irq_boost_work;
static struct delayed_work migt_rem;

static DEFINE_PER_CPU(struct smart_info, smart_info);

static cpumask_t migt_cluster_masks[3];
static int migt_cluster_mask_count;

static unsigned long *const clus_uid_lists[] = {
	cluster_affinity_uid_llist,
	cluster_affinity_uid_mlist,
	cluster_affinity_uid_blist,
};

static int set_mi_vip_task_req(const char *val,
			       const struct kernel_param *kp);
static int get_mi_viptask(char *buffer, const struct kernel_param *kp);
static int set_clus_affinity_uidlist(const char *val,
				     const struct kernel_param *kp);
static int get_clus_affinity_uidlist(char *buffer,
				     const struct kernel_param *kp);
static int set_migt_freq(const char *val, const struct kernel_param *kp);
static int get_migt_freq(char *buffer, const struct kernel_param *kp);

static enum hrtimer_restart do_boost_work(struct hrtimer *timer);
static void do_migt(struct work_struct *work);
static void do_migt_rem(struct work_struct *work);
static void update_policy_online(void);
static int boost_adjust_notify(struct notifier_block *nb, unsigned long val,
			       void *data);
static int ceiling_adjust_notify(struct notifier_block *nb, unsigned long val,
				 void *data);
static long migt_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
static int migt_mmap(struct file *file, struct vm_area_struct *vma);
static int migt_open(struct inode *inode, struct file *file);
static int migt_release(struct inode *inode, struct file *file);

void __weak mi_vip_task_req(unsigned int *uids, int num, unsigned int times)
{
}

int mi_uid_type(int uid)
{
	if ((unsigned int)uid >= MIGT_UID_MAX) {
		pr_info("smart-boost: Big uid %d warning\n", uid);
		return 0;
	}

	if (test_bit(uid, cluster_affinity_uid_mlist))
		return 2;
	if (test_bit(uid, cluster_affinity_uid_llist))
		return 1;
	if (test_bit(uid, cluster_affinity_uid_blist))
		return 3;

	return 0;
}
EXPORT_SYMBOL(mi_uid_type);

bool migt_is_render_thread(struct task_struct *p)
{
	return !strcmp(p->comm, "RenderThread");
}

void migt_wake_render(struct task_struct *p)
{
	if (minor_window_app && migt_is_render_thread(p))
		current->migt.wake_render++;
}

bool minor_window_task(struct task_struct *p)
{
	if (!minor_window_app)
		return false;

	return from_kuid(&init_user_ns, task_uid(p)) == minor_window_app;
}

bool get_minor_window_cpumask(struct task_struct *p, struct cpumask *mask)
{
	unsigned int uid;
	int idx, type;

	if (!migt_cluster_mask_count)
		return false;

	uid = from_kuid(&init_user_ns, task_uid(p));
	if (uid >= MIGT_UID_MAX)
		return false;

	type = mi_uid_type(uid);
	if (type <= 0)
		return false;

	idx = type - 1;
	if (idx >= migt_cluster_mask_count)
		idx = migt_cluster_mask_count - 1;

	cpumask_copy(mask, &migt_cluster_masks[idx]);
	cpumask_and(mask, mask, cpu_online_mask);

	return !cpumask_empty(mask);
}

void migt_monitor_hook(int enqueue, int cpu, struct task_struct *p,
		       u64 walltime)
{
}

unsigned int glk_freq_limit(struct cpufreq_policy *policy,
			    unsigned int *target_freq)
{
	struct smart_info *info = &per_cpu(smart_info, policy->cpu);
	unsigned int freq = *target_freq;

	if (info->boost_freq && freq < info->boost_freq)
		freq = info->boost_freq;
	if (info->ceiling_freq != (unsigned int)-1 && freq > info->ceiling_freq)
		freq = info->ceiling_freq;
	if (freq > policy->max)
		freq = policy->max;
	if (freq < policy->min)
		freq = policy->min;

	*target_freq = freq;

	return freq;
}

unsigned long glk_cal_freq(struct cpufreq_policy *policy, unsigned long util,
			   unsigned long max)
{
	return 0;
}

static void migt_build_cluster_masks(void)
{
	const struct cpumask *mask;
	unsigned int cpu;

	migt_cluster_mask_count = 0;

	for_each_possible_cpu(cpu) {
		mask = cpu_coregroup_mask(cpu);
		if (cpumask_first(mask) != cpu)
			continue;
		if (migt_cluster_mask_count >= ARRAY_SIZE(migt_cluster_masks))
			break;
		cpumask_copy(&migt_cluster_masks[migt_cluster_mask_count], mask);
		migt_cluster_mask_count++;
	}
}

static void add_uid_to_list(int uid, int type)
{
	if ((unsigned int)uid >= MIGT_UID_MAX) {
		pr_info("smart-boost: Big uid %d warning\n", uid);
		return;
	}

	switch (type) {
	case 1:
		clear_bit(uid, cluster_affinity_uid_mlist);
		clear_bit(uid, cluster_affinity_uid_blist);
		set_bit(uid, cluster_affinity_uid_llist);
		break;
	case 2:
		clear_bit(uid, cluster_affinity_uid_llist);
		clear_bit(uid, cluster_affinity_uid_blist);
		set_bit(uid, cluster_affinity_uid_mlist);
		break;
	case 3:
		clear_bit(uid, cluster_affinity_uid_llist);
		clear_bit(uid, cluster_affinity_uid_mlist);
		set_bit(uid, cluster_affinity_uid_blist);
		break;
	default:

		clear_bit(uid, cluster_affinity_uid_mlist);
		clear_bit(uid, cluster_affinity_uid_blist);
		clear_bit(uid, cluster_affinity_uid_llist);
		break;
	}

	if (tsched_debug)
		pr_info("smart-boost: Add uid %d to %d list\n", uid, type);
}

static void del_uid_from_list(int uid, int type)
{
	if ((unsigned int)uid >= MIGT_UID_MAX) {
		pr_info("smart-boost: Big uid %d warning\n", uid);
		return;
	}

	if (type >= 1 && type <= 3) {
		clear_bit(uid, clus_uid_lists[type - 1]);
	} else {
		clear_bit(uid, cluster_affinity_uid_blist);
		clear_bit(uid, cluster_affinity_uid_mlist);
		clear_bit(uid, cluster_affinity_uid_llist);
	}

	if (tsched_debug)
		pr_info("smart-boost: Del uid %d from %d list\n", uid, type);
}

static int set_clus_affinity_uidlist(const char *val,
				     const struct kernel_param *kp)
{
	const char *ptr, *end, *next;
	size_t len = strlen(kp->name);
	bool del = false, reset = false;
	unsigned int uid;
	int i, num, type = 0;

	if (strnstr(kp->name, "add_lclus_affinity_uidlist", len)) {
		type = 1;
	} else if (strnstr(kp->name, "add_mclus_affinity_uidlist", len)) {
		type = 2;
	} else if (strnstr(kp->name, "add_bclus_affinity_uidlist", len)) {
		type = 3;
	} else if (strnstr(kp->name, "reset_clus_affinity_uidlist", len)) {
		reset = true;
	} else if (strnstr(kp->name, "del_lclus_affinity_uidlist", len)) {
		del = true;
		type = 1;
	} else if (strnstr(kp->name, "del_mclus_affinity_uidlist", len)) {
		del = true;
		type = 2;
	} else if (strnstr(kp->name, "del_bclus_affinity_uidlist", len)) {
		del = true;
		type = 3;
	} else {
		return 0;
	}

	num = 0;
	for (ptr = val + 1; (next = strpbrk(ptr, ":")); ptr = next + 1)
		num++;

	if (!num) {
		if (sscanf(val, "%u\n", &uid) != 1)
			return -EINVAL;

		if (reset) {
			if (uid == 1)
				memset(cluster_affinity_uid_llist, 0,
				       sizeof(cluster_affinity_uid_llist));
			else if (uid == 3)
				memset(cluster_affinity_uid_blist, 0,
				       sizeof(cluster_affinity_uid_blist));
			else if (uid == 2)
				memset(cluster_affinity_uid_mlist, 0,
				       sizeof(cluster_affinity_uid_mlist));
			else {
				memset(cluster_affinity_uid_blist, 0,
				       sizeof(cluster_affinity_uid_blist));
				memset(cluster_affinity_uid_mlist, 0,
				       sizeof(cluster_affinity_uid_mlist));
				memset(cluster_affinity_uid_llist, 0,
				       sizeof(cluster_affinity_uid_llist));
			}
		} else if (del) {
			del_uid_from_list(uid, type);
		} else {
			add_uid_to_list(uid, type);
		}
		return 0;
	}

	ptr = val;
	end = val + strlen(val);

	for (i = 0; i < num; i++) {
		if (sscanf(ptr, "%u", &uid) != 1)
			return -EINVAL;

		if (del)
			del_uid_from_list(uid, type);
		else
			add_uid_to_list(uid, type);

		pr_info("smart-boost: arg %d val %d\n", i, uid);

		next = strpbrk(ptr + 1, ":");
		if (!next)
			return 0;
		ptr = next + 1;
		if (ptr >= end)
			return 0;
	}

	if (ptr >= end)
		return 0;
	if (sscanf(ptr, "%u", &uid) != 1)
		return -EINVAL;

	if (del)
		del_uid_from_list(uid, type);
	else
		add_uid_to_list(uid, type);
	pr_info("smart-boost: arg %d val = %d\n", num, uid);

	return 0;
}

static int get_clus_affinity_uidlist(char *buffer,
				     const struct kernel_param *kp)
{
	unsigned long *bitmap;
	unsigned int uid;
	size_t len = strlen(kp->name);
	int out = 0;

	if (strnstr(kp->name, "show_lclus_affinity_uidlist", len))
		bitmap = cluster_affinity_uid_llist;
	else if (strnstr(kp->name, "show_mclus_affinity_uidlist", len))
		bitmap = cluster_affinity_uid_mlist;
	else if (strnstr(kp->name, "show_bclus_affinity_uidlist", len))
		bitmap = cluster_affinity_uid_blist;
	else
		return scnprintf(buffer, PAGE_SIZE,
				 "need show_[b|l|m]clus_affinity_uidlist\n");

	for (uid = find_next_bit(bitmap, MIGT_UID_MAX, 0);
	     uid < MIGT_UID_MAX;
	     uid = find_next_bit(bitmap, MIGT_UID_MAX, uid + 1))
		out += scnprintf(buffer + out, PAGE_SIZE - out, "%u ", uid);

	out += scnprintf(buffer + out, PAGE_SIZE - out, "\n");
	return out;
}

static int set_mi_vip_task_req(const char *val,
			       const struct kernel_param *kp)
{
	const char *ptr, *end, *next;
	unsigned int uid = 0, times = 0;
	int i, num;

	num = 0;
	for (ptr = val + 1; (next = strpbrk(ptr, ":")); ptr = next + 1)
		num++;

	if (!num) {
		if (sscanf(val, "%u-%u\n", &uid, &times) != 2)
			return -EINVAL;

		pr_info("smart-boost: val %d times %d\n", uid, times);
		mi_vip_task_req(&uid, 1, times);
		return 0;
	}

	end = val + strlen(val);
	ptr = val;

	for (i = 0; i < num; i++) {
		if (sscanf(ptr, "%u", &uid) != 1)
			return -EINVAL;

		mi_viptask[i] = uid;
		pr_info("smart-boost: arg %d val %d\n", i + 1, uid);

		next = strpbrk(ptr + 1, ":");
		if (!next)
			return 0;
		ptr = next + 1;
		if (ptr >= end)
			return 0;

		if (i == 2) {
			const char *dash = strpbrk(ptr + 1, "-");

			if (!dash || dash + 1 >= end)
				return 0;
			if (sscanf(dash + 1, "%u", &times) != 1)
				return -EINVAL;

			pr_info("smart-boost: arg %d times %d\n", 3, times);
			mi_vip_task_req(mi_viptask, 3, times);
			return 0;
		}
	}

	if (sscanf(ptr, "%u-%u", &uid, &times) != 2)
		return -EINVAL;

	mi_viptask[num] = uid;
	pr_info("smart-boost: arg %d val = %d times = %d\n", num + 1, uid,
		times);
	mi_vip_task_req(mi_viptask, num + 1, times);
	return 0;
}

static int get_mi_viptask(char *buffer, const struct kernel_param *kp)
{
	int out = 0;
	int i, v;

	for (i = 0; i < 3; i++) {
		v = mi_vip_task_req_get ? mi_vip_task_req_get() : 0;
		out += scnprintf(buffer + out, PAGE_SIZE - out, "%u:%d ",
				 mi_viptask[i], v);
	}

	out += scnprintf(buffer + out, PAGE_SIZE - out, "\n");
	return out;
}

static int set_migt_freq(const char *val, const struct kernel_param *kp)
{
	struct smart_info *info;
	const char *ptr, *next;
	size_t len = strlen(kp->name);
	unsigned int cpu, freq;
	bool ceiling;
	int i, num;

	strnstr(kp->name, "migt_freq", len);
	ceiling = strnstr(kp->name, "smart_ceiling_freq", len) != NULL;

	num = 0;
	for (ptr = val + 1; (next = strpbrk(ptr, " :")); ptr = next + 1)
		num++;

	if (!num) {
		if (sscanf(val, "%u\n", &freq) != 1)
			return -EINVAL;

		for_each_possible_cpu(cpu) {
			info = &per_cpu(smart_info, cpu);
			if (ceiling)
				info->smart_ceiling_freq = freq;
			else
				info->migt_freq = freq;
		}
	} else {
		if (!(num & 1))
			return -EINVAL;

		ptr = val;
		for (i = 0; i < num; i += 2) {
			if (sscanf(ptr, "%u:%u", &cpu, &freq) != 2)
				return -EINVAL;

			if (cpu >= num_possible_cpus())
				return -EINVAL;

			info = &per_cpu(smart_info, cpu);
			if (ceiling)
				info->smart_ceiling_freq = freq;
			else
				info->migt_freq = freq;

			next = strnchr(ptr, strlen(ptr), ' ');
			if (!next) {
				if (i + 2 < num)
					return -EINVAL;
				break;
			}
			ptr = next + 1;
		}
	}

	migt_enabled = false;
	for_each_possible_cpu(cpu) {
		if (per_cpu(smart_info, cpu).migt_freq) {
			migt_enabled = true;
			break;
		}
	}

	return 0;
}

static int get_migt_freq(char *buffer, const struct kernel_param *kp)
{
	struct smart_info *info;
	unsigned int cpu;
	int out = 0;
	bool ceiling;

	strnstr(kp->name, "migt_freq", 20);
	ceiling = strnstr(kp->name, "smart_ceiling_freq", 20) != NULL;

	for_each_possible_cpu(cpu) {
		info = &per_cpu(smart_info, cpu);
		out += scnprintf(buffer + out, PAGE_SIZE - out, "%d:%u ", cpu,
				 ceiling ? info->smart_ceiling_freq :
					   info->migt_freq);
	}

	out += scnprintf(buffer + out, PAGE_SIZE - out, "\n");
	return out;
}

static const struct kernel_param_ops param_ops_mi_viptask = {
	.set = set_mi_vip_task_req,
	.get = get_mi_viptask,
};

static const struct kernel_param_ops param_ops_clus_affinity_uidlist = {
	.set = set_clus_affinity_uidlist,
	.get = get_clus_affinity_uidlist,
};

static const struct kernel_param_ops param_ops_migt_freq = {
	.set = set_migt_freq,
	.get = get_migt_freq,
};

module_param(high_resolution_enable, uint, 0644);
module_param(migt_debug, uint, 0644);
module_param(migt_ms, uint, 0644);
module_param(migt_thresh, uint, 0644);
module_param(minor_window_app, uint, 0644);
module_param(tsched_debug, uint, 0644);
module_param_cb(mi_viptask, &param_ops_mi_viptask, NULL, 0644);
module_param_cb(add_lclus_affinity_uidlist, &param_ops_clus_affinity_uidlist,
		NULL, 0644);
module_param_cb(add_mclus_affinity_uidlist, &param_ops_clus_affinity_uidlist,
		NULL, 0644);
module_param_cb(add_bclus_affinity_uidlist, &param_ops_clus_affinity_uidlist,
		NULL, 0644);
module_param_cb(reset_clus_affinity_uidlist, &param_ops_clus_affinity_uidlist,
		NULL, 0644);
module_param_cb(del_lclus_affinity_uidlist, &param_ops_clus_affinity_uidlist,
		NULL, 0644);
module_param_cb(del_mclus_affinity_uidlist, &param_ops_clus_affinity_uidlist,
		NULL, 0644);
module_param_cb(del_bclus_affinity_uidlist, &param_ops_clus_affinity_uidlist,
		NULL, 0644);
module_param_cb(show_lclus_affinity_uidlist, &param_ops_clus_affinity_uidlist,
		NULL, 0644);
module_param_cb(show_mclus_affinity_uidlist, &param_ops_clus_affinity_uidlist,
		NULL, 0644);
module_param_cb(show_bclus_affinity_uidlist, &param_ops_clus_affinity_uidlist,
		NULL, 0644);
module_param_cb(migt_freq, &param_ops_migt_freq, NULL, 0644);
module_param_cb(smart_ceiling_freq, &param_ops_migt_freq, NULL, 0644);

static enum hrtimer_restart do_boost_work(struct hrtimer *timer)
{
	if (!work_pending(&irq_boost_work))
		queue_work_on(WORK_CPU_UNBOUND, migt_wq, &irq_boost_work);

	return HRTIMER_NORESTART;
}

static void do_migt(struct work_struct *work)
{
	struct smart_info *info;
	unsigned int cpu;

	cancel_delayed_work_sync(&migt_rem);

	if (migt_debug)
		printk(KERN_ERR "smart-boost: smart boost comming....%d %llu",
		       __LINE__, (unsigned long long)ktime_to_us(ktime_get()));

	for_each_possible_cpu(cpu) {
		info = &per_cpu(smart_info, cpu);
		info->boost_freq = info->migt_freq;
	}

	update_policy_online();

	queue_delayed_work_on(WORK_CPU_UNBOUND, migt_wq, &migt_rem,
			      msecs_to_jiffies(migt_ms));
}

static void do_migt_rem(struct work_struct *work)
{
	struct smart_info *info;
	unsigned int cpu;

	if (migt_debug)
		printk(KERN_ERR "smart-boost: smart boost comming...%d %llu",
		       __LINE__, (unsigned long long)ktime_to_us(ktime_get()));

	for_each_possible_cpu(cpu) {
		info = &per_cpu(smart_info, cpu);
		info->boost_freq = 0;
	}

	update_policy_online();
}

static long migt_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct smart_info *info;
	unsigned int cmd_id = cmd & 0xff;
	unsigned int cpu;

	if (migt_debug)
		printk(KERN_ERR "smart-boost: smart boost comming. %d %d %llu",
		       __LINE__, cmd_id,
		       (unsigned long long)ktime_to_us(ktime_get()));

	switch (cmd_id) {
	case 1:
		if (!migt_enabled)
			break;

		if (high_resolution_enable) {
			if (hrtimer_active(&hrtimer))
				hrtimer_cancel(&hrtimer);
			hrtimer_start(&hrtimer, ms_to_ktime(migt_thresh),
				      HRTIMER_MODE_REL);
		} else {
			cancel_delayed_work_sync(&migt_work);
			queue_delayed_work_on(WORK_CPU_UNBOUND, migt_wq,
					      &migt_work,
					      msecs_to_jiffies(migt_thresh));
		}
		break;
	case 2:
		for_each_possible_cpu(cpu) {
			info = &per_cpu(smart_info, cpu);
			info->ceiling_freq = info->smart_ceiling_freq;
		}
		update_policy_online();
		break;
	case 3:
		for_each_possible_cpu(cpu) {
			info = &per_cpu(smart_info, cpu);
			info->ceiling_freq = -1;
		}
		update_policy_online();
		break;
	default:
		break;
	}

	return 0;
}

static int migt_mmap(struct file *file, struct vm_area_struct *vma)
{
	return 0;
}

static int migt_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int migt_release(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct file_operations migt_fops = {
	.unlocked_ioctl = migt_ioctl,
	.mmap = migt_mmap,
	.open = migt_open,
	.release = migt_release,
};

static struct miscdevice migt_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = MIGT_DEV_NAME,
	.fops = &migt_fops,
};

static void update_policy_online(void)
{
	struct cpufreq_policy *policy;
	unsigned int cpu;

	cpus_read_lock();

	for_each_online_cpu(cpu) {
		if (cpumask_first_and(cpu_coregroup_mask(cpu),
				      cpu_online_mask) != cpu)
			continue;

		if (migt_debug)
			pr_debug("smart-boost: Updating policy for CPU %d\n",
				 cpu);

		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;

		cpufreq_update_policy(cpu);
		cpufreq_cpu_put(policy);
	}

	cpus_read_unlock();
}

static int boost_adjust_notify(struct notifier_block *nb, unsigned long val,
			       void *data)
{
	struct cpufreq_policy *policy = data;
	struct smart_info *info;
	unsigned int cpu, boost;

	if (val != CPUFREQ_ADJUST)
		return NOTIFY_OK;

	cpu = policy->cpu;
	info = &per_cpu(smart_info, cpu);
	boost = info->boost_freq;
	if (!boost)
		return NOTIFY_OK;

	if (migt_debug) {
		pr_debug("smart-boost: CPU%u policy min before boost: %u kHz\n",
			 cpu, policy->min);
		pr_debug("smart-boost: CPU%u boost min: %u kHz\n", cpu, boost);
	}

	if (policy->min < boost)
		policy->min = boost;
	if (policy->max < boost)
		policy->max = boost;
	if (policy->min > policy->max)
		policy->min = policy->max;

	if (migt_debug)
		pr_debug("smart-boost: CPU%u policy min after boost: %u kHz\n",
			 cpu, policy->min);

	return NOTIFY_OK;
}

static int ceiling_adjust_notify(struct notifier_block *nb, unsigned long val,
				 void *data)
{
	struct cpufreq_policy *policy = data;
	struct smart_info *info;
	unsigned int cpu, ceiling;

	if (val != CPUFREQ_ADJUST)
		return NOTIFY_OK;

	cpu = policy->cpu;
	info = &per_cpu(smart_info, cpu);
	ceiling = info->ceiling_freq;
	if (!ceiling)
		return NOTIFY_OK;

	if (migt_debug) {
		pr_debug("smart-boost: CPU%u policy max before boost: %u kHz\n",
			 cpu, policy->max);
		pr_debug("smart-boost: CPU%u boost max: %u kHz\n", cpu,
			 ceiling);
	}

	if (policy->min > ceiling)
		policy->min = ceiling;
	if (policy->max > ceiling)
		policy->max = ceiling;
	if (policy->min > policy->max)
		policy->min = policy->max;

	if (migt_debug)
		pr_debug("smart-boost: CPU%u policy max after boost: %u kHz\n",
			 cpu, policy->max);

	return NOTIFY_OK;
}

static struct notifier_block boost_adjust_nb = {
	.notifier_call = boost_adjust_notify,
};

static struct notifier_block ceiling_adjust_nb = {
	.notifier_call = ceiling_adjust_notify,
};

static int migt_init(void)
{
	struct smart_info *info;
	unsigned int cpu;
	int ret;

	hrtimer_init(&hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hrtimer.function = do_boost_work;

	migt_wq = alloc_workqueue("migt_wq", WQ_HIGHPRI, 0);
	if (!migt_wq)
		return -EFAULT;

	ret = misc_register(&migt_misc);
	if (ret) {
		destroy_workqueue(migt_wq);
		migt_wq = NULL;
		return ret;
	}

	INIT_DELAYED_WORK(&migt_work, do_migt);
	INIT_WORK(&irq_boost_work, do_migt);
	INIT_DELAYED_WORK(&migt_rem, do_migt_rem);

	for_each_possible_cpu(cpu) {
		info = &per_cpu(smart_info, cpu);
		info->cpu = cpu;
		info->ceiling_freq = -1;
	}

	migt_build_cluster_masks();

	cpufreq_register_notifier(&boost_adjust_nb, CPUFREQ_POLICY_NOTIFIER);
	cpufreq_register_notifier(&ceiling_adjust_nb, CPUFREQ_POLICY_NOTIFIER);

	return 0;
}
late_initcall(migt_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MIGT (Mi Game Turbo) driver");
