/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_CPUFREQ_H
#define _LINUX_SCHED_CPUFREQ_H

#include <linux/types.h>

/*
 * Interface between cpufreq drivers and the scheduler:
 */

#define SCHED_CPUFREQ_RT	(1U << 0)
#define SCHED_CPUFREQ_DL	(1U << 1)
#define SCHED_CPUFREQ_IOWAIT	(1U << 2)
#define SCHED_CPUFREQ_INTERCLUSTER_MIG (1U << 3)
#define SCHED_CPUFREQ_WALT (1U << 4)
#define SCHED_CPUFREQ_PL	(1U << 5)
#define SCHED_CPUFREQ_EARLY_DET (1U << 6)
#define SCHED_CPUFREQ_FORCE_UPDATE (1U << 7)
#define SCHED_CPUFREQ_CONTINUE (1U << 8)

#define SCHED_CPUFREQ_RT_DL	(SCHED_CPUFREQ_RT | SCHED_CPUFREQ_DL)

/*
 * Reasons for a frequency request, reported by the WALT load helpers and
 * used by the walt governor for tracing and tuning.
 */
#define CPUFREQ_REASON_LOAD		0
#define CPUFREQ_REASON_BTR		0x1
#define CPUFREQ_REASON_PL		0x2
#define CPUFREQ_REASON_EARLY_DET	0x4
#define CPUFREQ_REASON_RTG_BOOST	0x8
#define CPUFREQ_REASON_HISPEED		0x10
#define CPUFREQ_REASON_NWD		0x20
#define CPUFREQ_REASON_FREQ_AGR		0x40
#define CPUFREQ_REASON_KSOFTIRQD	0x80
#define CPUFREQ_REASON_TT_LOAD		0x100
#define CPUFREQ_REASON_SUH		0x200
#define CPUFREQ_REASON_ADAPTIVE_LOW	0x400
#define CPUFREQ_REASON_ADAPTIVE_HIGH	0x800

#ifdef CONFIG_CPU_FREQ
struct update_util_data {
       void (*func)(struct update_util_data *data, u64 time, unsigned int flags);
};

void cpufreq_add_update_util_hook(int cpu, struct update_util_data *data,
                       void (*func)(struct update_util_data *data, u64 time,
				    unsigned int flags));
void cpufreq_remove_update_util_hook(int cpu);
#endif /* CONFIG_CPU_FREQ */

#endif /* _LINUX_SCHED_CPUFREQ_H */
