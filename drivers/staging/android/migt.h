#ifndef _MIGT_H
#define _MIGT_H

#include <linux/types.h>

#define MIGT_UID_MAX		200000

#define MIGT_DEV_NAME		"migt"

#ifndef MIGT_MINOR_TASK
#define MIGT_MINOR_TASK		0x1
#endif

struct smart_info {
	unsigned int cpu;
	unsigned int boost_freq;
	unsigned int migt_freq;
	unsigned int ceiling_freq;
	unsigned int smart_ceiling_freq;
};

extern unsigned int minor_window_app;
extern unsigned int mi_viptask[3];

int mi_uid_type(int uid);

void mi_vip_task_req(unsigned int *uids, int num, unsigned int times);

struct task_struct;
struct cpufreq_policy;
struct cpumask;

void migt_monitor_hook(int enqueue, int cpu, struct task_struct *p,
		       u64 walltime);
unsigned int glk_freq_limit(struct cpufreq_policy *policy,
			    unsigned int *target_freq);
unsigned long glk_cal_freq(struct cpufreq_policy *policy, unsigned long util,
			   unsigned long max);
bool minor_window_task(struct task_struct *p);
bool get_minor_window_cpumask(struct task_struct *p, struct cpumask *mask);
bool migt_is_render_thread(struct task_struct *p);
void migt_wake_render(struct task_struct *p);

#endif
