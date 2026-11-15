/* SPDX-License-Identifier: GPL-2.0 */
/*
 * millet reporting framework interface.
 */
#ifndef __LINUX_MILLET_H__
#define __LINUX_MILLET_H__

#include <linux/types.h>
#include <linux/spinlock.h>

struct task_struct;
struct cgroup_taskset;
struct sock;
struct millet;
struct millet_msg;

struct millet_channel {
	u32 id;					/* 0x00 */
	u32 pad0;				/* 0x04 */
	void *priv;				/* 0x08 */
	u32 pad1;				/* 0x10 */
	u32 out_id;				/* 0x14 */
	u32 portid;				/* 0x18 */
	u32 pad2;				/* 0x1c */
	int (*recv_from)(void *msg, u32 len);	/* 0x20 */
	int (*send_to)(struct task_struct *task, struct millet_msg *msg,
		       struct millet *millet);	/* 0x28 */
	void (*init)(struct millet *millet);	/* 0x30 */
	spinlock_t lock;			/* 0x38 */
	u64 send_suc;				/* 0x40 */
	u64 send_fail;				/* 0x48 */
	u64 runtime_us;				/* 0x50 */
	char name[0x18];			/* 0x58 */
};

struct millet {
	u32 enable;				/* 0x00 */
	u32 pad0;				/* 0x04 */
	struct sock *nl_sk;			/* 0x08 */
	u8 pad1[0x60];				/* 0x10 */
	struct millet_channel ch[6];		/* 0x70 */
};

struct millet_msg {
	u32 module;			/* 0x00 */
	u32 type;			/* 0x04 */
	u32 msg;			/* 0x08, 1 ack, 2 report, 3 cmd */
	u32 pad0;			/* 0x0c */
	u64 src_port;			/* 0x10 */
	u64 dst_port;			/* 0x18 */
	u32 uid;			/* 0x20 */
	u32 pad1;			/* 0x24 */
	u64 ts_sec;			/* 0x28 */
	u64 ts_nsec;			/* 0x30 */
	u32 subtype;			/* 0x38 */
	u8 pad2[0x0c];			/* 0x3c */
	u32 field_48;			/* 0x48 */
	u32 field_4c;			/* 0x4c */
	u64 pad3[3];			/* 0x50 */
	union {
		struct task_struct *task;
		u32 uid;
		u64 val;
	} field_68;			/* 0x68 */
	union {
		struct task_struct *task;
		struct {
			u32 tgid;
			u32 tid;
		};
		u64 val;
	} field_70;			/* 0x70 */
	u32 status;			/* 0x78 */
	u32 pid;			/* 0x7c */
	u32 from_pid;			/* 0x80 */
	u32 peer_pid;			/* 0x84 */
	u8 flags;			/* 0x88 */
	u8 pad4[3];			/* 0x89 */
	u32 code;			/* 0x8c */
};

struct millet_cmd {
	u32 module;			/* 0x00 */
	u32 type;			/* 0x04 */
	u64 dst_token;			/* 0x08 */
	u64 src_token;			/* 0x10 */
	u8 pad0[0x30];			/* 0x18 */
	u32 cmd;			/* 0x48 */
	u32 uid;			/* 0x4c */
};

#define MILLET_MOD_SIG			1
#define MILLET_MOD_BINDER		2
#define MILLET_MOD_BINDER_STAT		3
#define MILLET_MOD_MEM			4
#define MILLET_MOD_PKG			5
#define MILLET_MOD_HANDSHK		6
#define MILLET_MOD_MAX			6

#define MILLET_MSG_ACK			1
#define MILLET_MSG_REPORT		2
#define MILLET_MSG_CMD			3
#define MILLET_MSG_MAX			4

#define MILLET_MAGIC_SRC		0x12341234U
#define MILLET_MAGIC_DST		0xabcddcbaU

#define MILLET_NETLINK_UNIT		29
#define MILLET_UID_REC_MAX		64
#define MILLET_FROZEN_UID_DEFAULT	10000
#define MILLET_NF_PRIORITY		226

#define MILLET_PKG_ADD			1
#define MILLET_PKG_DEL			2
#define MILLET_PKG_CLEAR		3

#define MILLET_BINDER_SUB_REPLY		1
#define MILLET_BINDER_SUB_CALL		2
#define MILLET_BINDER_SUB_WAIT		3
#define MILLET_BINDER_SUB_STACK		4

int register_millet_hook(u32 type,
			 int (*recv_from)(void *msg, u32 len),
			 int (*send_to)(struct task_struct *task,
					struct millet_msg *msg,
					struct millet *millet),
			 void (*init)(struct millet *millet));
int unregister_millet_hook(u32 type);
int millet_sendmsg(u32 type, struct task_struct *task, struct millet_msg *msg);
int millet_sendto_user(struct task_struct *task, struct millet_msg *msg,
		       struct millet *millet);
int millet_can_attach(struct cgroup_taskset *tset);

extern unsigned int frozen_uid_min;

#endif /* __LINUX_MILLET_H__ */
