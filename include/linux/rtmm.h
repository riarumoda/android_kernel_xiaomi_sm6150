#ifndef __RTMM_H__
#define __RTMM_H__

#include <linux/types.h>

extern bool rtmm_reclaim(const char *name);
extern int rtmm_reclaim_swappiness(void);

#endif
