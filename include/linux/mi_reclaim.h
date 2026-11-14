/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mi_reclaim interface.
 *
 * The reclaim primitive mi_reclaim_global() lives in mm/vmscan.c because
 * it needs struct scan_control and the static do_try_to_free_pages().
 */
#ifndef __LINUX_MI_RECLAIM_H__
#define __LINUX_MI_RECLAIM_H__

#ifdef CONFIG_MI_RECLAIM

extern bool mi_reclaim(const char *name);
extern bool mi_st(void);
extern int mi_reclaim_swappiness(void);
extern unsigned long mi_reclaim_global(unsigned long nr_to_reclaim,
				       int page_type);

#endif /* CONFIG_MI_RECLAIM */

#endif /* __LINUX_MI_RECLAIM_H__ */
