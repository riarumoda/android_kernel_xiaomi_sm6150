/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Panel physical size override interface.
 */
#ifndef _LINUX_PANEL_OVERRIDE_H
#define _LINUX_PANEL_OVERRIDE_H

#include <linux/types.h>

#ifdef CONFIG_PANEL_OVERRIDE
int panel_override_set(u32 width_mm, u32 height_mm);
void panel_override_apply(u32 *width_mm, u32 *height_mm);
#else
static inline int panel_override_set(u32 width_mm, u32 height_mm)
{
	return 0;
}

static inline void panel_override_apply(u32 *width_mm, u32 *height_mm)
{
}
#endif

#endif /* _LINUX_PANEL_OVERRIDE_H */
