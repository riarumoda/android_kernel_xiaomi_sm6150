/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DSU (Dynamic System Update) detection interface.
 */
#ifndef _LINUX_DSU_DETECT_H
#define _LINUX_DSU_DETECT_H

#include <linux/types.h>

#ifdef CONFIG_DSU_DETECT
/* May sleep: checks the backing device of the root filesystem. */
bool dsu_detect_active(void);
void dsu_detect_note_dm_name(const char *name, dev_t dev);
#else
static inline bool dsu_detect_active(void)
{
	return false;
}

static inline void dsu_detect_note_dm_name(const char *name, dev_t dev)
{
}
#endif

#endif /* _LINUX_DSU_DETECT_H */
