/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UFSFEATURE_IOCTL_H_
#define _UFSFEATURE_IOCTL_H_

#include <linux/types.h>

#define UFSFEATURE_QUERY_OPCODE		0x5500

struct ufs_ioctl_query_data_hpb {
	__u32 opcode;
	__u8 idn;
	__u16 buf_size;
	__u8 buffer[0];
};

#endif /* _UFSFEATURE_IOCTL_H_ */
