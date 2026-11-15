// SPDX-License-Identifier: GPL-2.0
/*
 * millet reporting framework.
 *
 * One netlink socket and six module slots.  A module registers an optional
 * receive callback (user -> kernel command), a send callback (kernel ->
 * user report) and an init callback.  The signal and binder integration
 * lives in their own translation units.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/spinlock.h>
#include <linux/ktime.h>
#include <linux/skbuff.h>
#include <linux/netlink.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/socket.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/cred.h>
#include <linux/freezer.h>
#include <linux/cgroup.h>
#include <linux/capability.h>
#include <linux/atomic.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/netfilter_ipv6.h>
#include <net/net_namespace.h>
#include <net/netlink.h>
#include <net/sock.h>
#include <net/tcp_states.h>
#include <linux/ip.h>
#include <net/ipv6.h>

#include <linux/millet.h>

static int millet_debug;
module_param(millet_debug, int, 0644);

unsigned int frozen_uid_min = MILLET_FROZEN_UID_DEFAULT;
module_param(frozen_uid_min, uint, 0644);

static struct millet millet_sk;
static struct proc_dir_entry *millet_rootdir;
static unsigned int millet_v = 1;
static u32 uid_rec[MILLET_UID_REC_MAX];

int register_millet_hook(u32 type,
			 int (*recv_from)(void *msg, u32 len),
			 int (*send_to)(struct task_struct *task,
					struct millet_msg *msg,
					struct millet *millet),
			 void (*init)(struct millet *millet))
{
	struct millet_channel *ch;

	if (type < MILLET_MOD_SIG || type > MILLET_MOD_MAX) {
		pr_err("millet: %s: type is invalid! %d\n", __func__, type);
		return -1;
	}

	ch = &millet_sk.ch[type - 1];
	if (ch->init) {
		pr_err("millet: %s: init has been registered %d\n",
		       __func__, type);
		return -1;
	}
	ch->init = init;

	if (ch->recv_from) {
		pr_err("millet: %s: recv_from has been registered %d\n",
		       __func__, type);
		return -1;
	}
	ch->recv_from = recv_from;

	if (ch->send_to) {
		pr_err("millet: %s: send_to has been registered %d\n",
		       __func__, type);
		return -1;
	}
	ch->send_to = send_to;

	if (millet_debug) {
		printk("type %d register hook\n", type);
		dump_stack();
	}

	return 0;
}

int unregister_millet_hook(u32 type)
{
	struct millet_channel *ch;

	if (type < MILLET_MOD_SIG || type > MILLET_MOD_MAX) {
		pr_err("millet: %s: type is invalid! %d\n", __func__, type);
		return -1;
	}

	ch = &millet_sk.ch[type - 1];
	ch->recv_from = NULL;
	ch->send_to = NULL;
	ch->init = NULL;

	return 0;
}

static void dump_send_msg(struct millet_msg *msg)
{
	pr_info("millet: -----up-report msg-head dump-----\n");
	if (!msg) {
		pr_err("millet: msg is NULL");
		return;
	}
	pr_info("millet: msg: %d\n", msg->msg);
	pr_info("millet: type: %d\n", msg->module);
	pr_info("millet: src_port: 0x%x\n", (u32)msg->src_port);
	pr_info("millet: dest_port: 0x%x\n", (u32)msg->dst_port);
	pr_info("millet: uid: %d\n", msg->uid);
}

int millet_sendto_user(struct task_struct *task, struct millet_msg *msg,
		       struct millet *millet)
{
	struct timespec64 ts;
	struct millet_msg *payload;
	struct nlmsghdr *nlh;
	struct sk_buff *skb;
	u32 module, resolved;
	int ret;

	(void)task;

	if (!millet->enable)
		return -1;

	if (!msg || msg->msg < MILLET_MSG_ACK || msg->msg > MILLET_MSG_MAX) {
		pr_err("millet: %s:msg or  msg type is invalid! %d\n",
		       __func__, msg ? msg->msg : 0);
		return -1;
	}

	module = msg->module;
	if (module < MILLET_MOD_SIG || module > MILLET_MOD_MAX)
		return -1;

	skb = __alloc_skb(0xa0, GFP_ATOMIC, 0, -1);
	if (!skb) {
		pr_err("millet: %s alloc_skb failed! %d\n", __func__, module);
		return -1;
	}

	if (skb->data_len || skb_tailroom(skb) < 0xa0) {
		kfree_skb(skb);
		return -1;
	}

	nlh = __nlmsg_put(skb, 0, 0, 0, sizeof(*msg), 0);
	if (!nlh) {
		kfree_skb(skb);
		return -1;
	}

	payload = nlmsg_data(nlh);
	memset(payload, 0, sizeof(*payload));
	memcpy(payload, msg, sizeof(*payload));

	payload->src_port = MILLET_MAGIC_SRC;
	payload->dst_port = MILLET_MAGIC_DST;

	resolved = module;
	if (module >= MILLET_MOD_SIG && module <= MILLET_MOD_MAX) {
		u32 out_id = millet->ch[module - 1].out_id;

		if (out_id >= MILLET_MOD_SIG && out_id <= MILLET_MOD_MAX)
			resolved = out_id;
	}
	payload->type = resolved;

	ktime_get_ts64(&ts);
	payload->ts_sec = ts.tv_sec;
	payload->ts_nsec = ts.tv_nsec;

	if (millet_debug)
		dump_send_msg(payload);

	ret = netlink_unicast(millet->nl_sk, skb,
			      millet->ch[resolved - 1].portid,
			      MSG_DONTWAIT);
	if (ret < 0) {
		pr_err("millet: nlmsg_unicast failed! %s errno %d\n",
		       __func__, ret);
		return -1;
	}

	if (millet_debug)
		pr_info("millet: nlmsg_unicast snd msg success\n");

	return 0;
}

int millet_sendmsg(u32 type, struct task_struct *task, struct millet_msg *msg)
{
	struct millet_channel *ch;
	ktime_t start;
	unsigned long flags;
	int ret;

	if (type < MILLET_MOD_SIG || type > MILLET_MOD_MAX) {
		pr_err("millet: wrong type valid %d\n", type);
		return -1;
	}

	ch = &millet_sk.ch[type - 1];
	if (!ch->send_to) {
		pr_err("millet: mod %d send_to interface is NULL", type);
		return -1;
	}

	start = ktime_get();
	ret = ch->send_to(task, msg, &millet_sk);

	spin_lock_irqsave(&ch->lock, flags);
	if (ret < 0)
		ch->send_fail++;
	else if (ret > 0)
		ch->send_suc++;
	ch->runtime_us += ktime_to_us(ktime_get()) - ktime_to_us(start);
	spin_unlock_irqrestore(&ch->lock, flags);

	return ret;
}

static void dump_recv_msg(struct millet_cmd *cmd)
{
	if (!millet_debug)
		return;

	pr_info("millet: -----recv msg from user-----\n");
	if (!cmd) {
		pr_err("millet: msg is NULL");
		return;
	}
	pr_info("millet: msg: %d\n", cmd->type);
	pr_info("millet: type: %d\n", cmd->module);
	pr_info("millet: src_port: 0x%x\n", (u32)cmd->dst_token);
	pr_info("millet: dest_port: 0x%x\n", (u32)cmd->src_token);
}

static void millet_recv_handler(struct sk_buff *skb)
{
	struct millet_channel *ch;
	struct millet_cmd *cmd;
	struct nlmsghdr *nlh;
	u32 module, type, seq;

	if (!skb) {
		pr_err("millet: recv_handler %s: skb is\tNULL!\n", __func__);
		return;
	}

	if (NETLINK_CB(skb).creds.uid.val >= 1001) {
		pr_err("millet: uid: %d, permission denied\n",
		       NETLINK_CB(skb).creds.uid.val);
		return;
	}

	if (millet_debug)
		pr_info("millet: kernel millet receive msg now\n");

	if (skb->len <= 0xf) {
		pr_err("millet: msg len is invalid\n");
		return;
	}

	nlh = nlmsg_hdr(skb);
	if (nlh->nlmsg_len <= 0x5f) {
		pr_err("millet: msg len err %d need %d\n",
		       nlh->nlmsg_len, 0x60);
		return;
	}

	cmd = nlmsg_data(nlh);
	if (cmd->dst_token != MILLET_MAGIC_DST) {
		pr_err("millet: src_port %x is not valid!\n",
		       (u32)cmd->dst_token);
		return;
	}
	if (cmd->src_token != MILLET_MAGIC_SRC) {
		pr_err("millet: dst_port is %x not kernel!\n",
		       (u32)cmd->src_token);
		return;
	}

	module = cmd->module;
	if (module < MILLET_MOD_SIG || module > MILLET_MOD_MAX) {
		pr_err("millet: mod %d is not valid!\n", module);
		return;
	}

	type = cmd->type;
	seq = nlh->nlmsg_seq;

	if (type == MILLET_MSG_CMD) {
		ch = &millet_sk.ch[module - 1];
		if (ch->recv_from)
			ch->recv_from(cmd, 0x50);
		if (millet_debug) {
			printk("millet: recv mesg form %d\n", seq);
			dump_recv_msg(cmd);
		}
		return;
	}

	if (type == MILLET_MSG_ACK) {
		struct millet_msg meta;

		millet_sk.ch[module - 1].portid = seq;
		memset(&meta, 0, sizeof(meta));
		meta.module = module;
		meta.msg = MILLET_MSG_ACK;
		dump_recv_msg(cmd);
		millet_sendto_user(NULL, &meta, &millet_sk);
		return;
	}

	pr_err("millet: msg type is valid %d\n", type);
}

static int millet_sock_show(struct seq_file *m, void *v)
{
	u64 suc[MILLET_MOD_MAX + 1] = { };
	u64 fail[MILLET_MOD_MAX + 1] = { };
	u64 runtime[MILLET_MOD_MAX + 1] = { };
	u64 total_suc = 0, total_fail = 0, total_runtime = 0;
	unsigned long flags;
	int i;

	for (i = 0; i < MILLET_MOD_MAX; i++) {
		struct millet_channel *ch = &millet_sk.ch[i];

		spin_lock_irqsave(&ch->lock, flags);
		suc[i + 1] = ch->send_suc;
		fail[i + 1] = ch->send_fail;
		runtime[i + 1] = ch->runtime_us;
		spin_unlock_irqrestore(&ch->lock, flags);
	}

	seq_printf(m, "-----------------------------\n\n");

	for (i = 1; i <= MILLET_MOD_MAX; i++) {
		if (!suc[i] && !fail[i])
			continue;

		seq_printf(m, "name: %s mod id %d:\n",
			   millet_sk.ch[i - 1].name, i);
		seq_printf(m, "send suc: %llu\n", suc[i]);
		seq_printf(m, "send fail: %llu\n", fail[i]);
		seq_printf(m, "runtime: %llu us\n\n", runtime[i]);

		total_suc += suc[i];
		total_fail += fail[i];
		total_runtime += runtime[i];
	}

	seq_printf(m, "---------total info--------------------\n");
	seq_printf(m, "send_suc: %llu\n", total_suc);
	seq_printf(m, "send_fail: %llu\n", total_fail);
	seq_printf(m, "runtime: %llu us\n", total_runtime);

	return 0;
}

static int millet_stat_open(struct inode *inode, struct file *file)
{
	return single_open(file, millet_sock_show, NULL);
}

static ssize_t millet_stat_write(struct file *file, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	char value[8] = { 0 };
	char c;
	long val;
	int i;

	if (count) {
		if (copy_from_user(&c, buf, 1))
			return -EFAULT;
		value[0] = c;
	}

	val = simple_strtol(value, NULL, 10);
	pr_info("millet: input value number: %d\n", (int)val);

	if (val == 1) {
		for (i = 0; i < MILLET_MOD_MAX; i++) {
			struct millet_channel *ch = &millet_sk.ch[i];
			unsigned long flags;

			spin_lock_irqsave(&ch->lock, flags);
			ch->send_suc = 0;
			ch->send_fail = 0;
			ch->runtime_us = 0;
			spin_unlock_irqrestore(&ch->lock, flags);
		}
	}

	pr_warn("stat reset now\n");

	return count;
}

static int millet_version_show(struct seq_file *m, void *v);

static int millet_version_open(struct inode *inode, struct file *file)
{
	return single_open(file, millet_version_show, NULL);
}

static int millet_version_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", millet_v);
	return 0;
}

static const struct file_operations millet_proc_fops = {
	.owner		= THIS_MODULE,
	.llseek		= seq_lseek,
	.read		= seq_read,
	.write		= millet_stat_write,
	.open		= millet_stat_open,
	.release	= single_release,
};

static const struct file_operations millet_version_fops = {
	.owner		= THIS_MODULE,
	.llseek		= seq_lseek,
	.read		= seq_read,
	.open		= millet_version_open,
	.release	= single_release,
};

#if IS_ENABLED(CONFIG_NETFILTER)

static void pkg_report_uid(u32 uid, const char *name)
{
	struct millet_msg msg;
	int i;

	for (i = 0; i < MILLET_UID_REC_MAX; i++) {
		if (uid_rec[i] != uid)
			continue;
		if (cmpxchg(&uid_rec[i], uid, 0) != uid)
			return;

		memset(&msg, 0, sizeof(msg));
		msg.field_68.uid = uid;
		if (millet_sendmsg(MILLET_MOD_PKG, current, &msg) < 0)
			pr_err("%s : up report failed!\n", name);
		return;
	}
}

static unsigned int pkg_check_sock(struct sock *sk, const char *name)
{
	struct inode *inode;
	u32 uid;

	if (!sk)
		return NF_ACCEPT;

	if (sk->sk_state == TCP_NEW_SYN_RECV) {
		sk = sk->__sk_common.skc_listener;
		if (!sk)
			return NF_ACCEPT;
	}

	if (sk->sk_state == TCP_TIME_WAIT ||
	    sk->sk_state == TCP_NEW_SYN_RECV)
		return NF_ACCEPT;

	if (!sk->sk_socket)
		return NF_ACCEPT;

	inode = SOCK_INODE(sk->sk_socket);
	uid = inode->i_uid.val;

	/* report only for uids at or above 10000 */
	if (uid < 10000)
		return NF_ACCEPT;

	pkg_report_uid(uid, name);

	return NF_ACCEPT;
}

static unsigned int pkg_ip4_in(void *priv, struct sk_buff *skb,
			       const struct nf_hook_state *state)
{
	const struct iphdr *iph;

	iph = ip_hdr(skb);
	if (iph->protocol != IPPROTO_TCP)
		return NF_ACCEPT;

	return pkg_check_sock(skb->sk, __func__);
}

#if IS_ENABLED(CONFIG_IPV6)
static unsigned int pkg_ip6_in(void *priv, struct sk_buff *skb,
			       const struct nf_hook_state *state)
{
	unsigned int offset = 0;
	unsigned short fragoff = 0;

	if (ipv6_find_hdr(skb, &offset, -1, &fragoff, NULL) != IPPROTO_TCP)
		return NF_ACCEPT;

	return pkg_check_sock(skb->sk, __func__);
}
#endif

static unsigned int pkg_ip4_out(void *priv, struct sk_buff *skb,
			       const struct nf_hook_state *state)
{
	return NF_ACCEPT;
}

#if IS_ENABLED(CONFIG_IPV6)
static unsigned int pkg_ip6_out(void *priv, struct sk_buff *skb,
			       const struct nf_hook_state *state)
{
	return NF_ACCEPT;
}
#endif

static const struct nf_hook_ops pkg_nf_ops[] = {
	{
		.hook		= pkg_ip4_in,
		.pf		= NFPROTO_IPV4,
		.hooknum	= NF_INET_LOCAL_IN,
		.priority	= MILLET_NF_PRIORITY,
	},
#if IS_ENABLED(CONFIG_IPV6)
	{
		.hook		= pkg_ip6_in,
		.pf		= NFPROTO_IPV6,
		.hooknum	= NF_INET_LOCAL_IN,
		.priority	= MILLET_NF_PRIORITY,
	},
#endif
	{
		.hook		= pkg_ip4_out,
		.pf		= NFPROTO_IPV4,
		.hooknum	= NF_INET_LOCAL_OUT,
		.priority	= MILLET_NF_PRIORITY,
	},
#if IS_ENABLED(CONFIG_IPV6)
	{
		.hook		= pkg_ip6_out,
		.pf		= NFPROTO_IPV6,
		.hooknum	= NF_INET_LOCAL_OUT,
		.priority	= MILLET_NF_PRIORITY,
	},
#endif
};

static int pkg_recv_hook(void *msg, u32 len)
{
	struct millet_cmd *cmd = msg;
	u32 uid = cmd->uid;
	int i, slot = MILLET_UID_REC_MAX;

	switch (cmd->cmd) {
	case MILLET_PKG_ADD:
		for (i = 0; i < MILLET_UID_REC_MAX; i++) {
			if (uid_rec[i] == uid)
				return 0;
			if (slot == MILLET_UID_REC_MAX && !uid_rec[i])
				slot = i;
		}
		if (slot >= MILLET_UID_REC_MAX) {
			pr_err("%s : add uid:%d failed (full)!\n",
			       "pkg_add_uid", uid);
			return 0;
		}
		uid_rec[slot] = uid;
		break;
	case MILLET_PKG_DEL:
		for (i = 0; i < MILLET_UID_REC_MAX; i++) {
			if (uid_rec[i] == uid) {
				uid_rec[i] = 0;
				break;
			}
		}
		break;
	case MILLET_PKG_CLEAR:
		for (i = 0; i < MILLET_UID_REC_MAX; i++)
			uid_rec[i] = 0;
		break;
	}

	return 0;
}

static int pkg_sendmsg(struct task_struct *task, struct millet_msg *msg,
		       struct millet *millet)
{
	if (!task || !msg || !millet) {
		pr_err("%s input invalid\n", __func__);
		return -1;
	}

	msg->msg = MILLET_MSG_REPORT;
	msg->module = MILLET_MOD_PKG;
	msg->uid = msg->field_68.uid;

	return millet_sendto_user(task, msg, millet);
}

static void pkg_init_millet(struct millet *millet)
{
	millet->ch[MILLET_MOD_PKG - 1].out_id = MILLET_MOD_PKG;
}

static int __init millet_pkg_init(void)
{
	int ret;

	memset(uid_rec, 0, sizeof(uid_rec));

	ret = nf_register_net_hooks(&init_net, pkg_nf_ops,
				    ARRAY_SIZE(pkg_nf_ops));
	if (ret < 0) {
		pr_err("nf_register_hooks(millet hooks) error\n");
		return -1;
	}

	pr_err("nf_register_hooks(millet hooks) success\n");

	register_millet_hook(MILLET_MOD_PKG, pkg_recv_hook, pkg_sendmsg,
			     pkg_init_millet);

	return 0;
}
device_initcall(millet_pkg_init);

#endif /* CONFIG_NETFILTER */

static int hs_recv_hook(void *msg, u32 len)
{
	struct millet_msg meta;

	memset(&meta, 0, sizeof(meta));

	return millet_sendmsg(MILLET_MOD_HANDSHK, current, &meta);
}

static int hs_sendmsg(struct task_struct *task, struct millet_msg *msg,
		      struct millet *millet)
{
	if (!msg || !millet) {
		pr_err("%s input invalid\n", __func__);
		return -1;
	}

	msg->msg = MILLET_MSG_REPORT;
	msg->module = MILLET_MOD_HANDSHK;

	return millet_sendto_user(task, msg, millet);
}

static void hs_init_millet(struct millet *millet)
{
	millet->ch[MILLET_MOD_HANDSHK - 1].out_id = MILLET_MOD_SIG;
}

static int __init millet_hs_init(void)
{
	pr_info("hs_register_hooks(millet hooks) success\n");

	register_millet_hook(MILLET_MOD_HANDSHK, hs_recv_hook, hs_sendmsg,
			     hs_init_millet);

	return 0;
}
device_initcall(millet_hs_init);

int millet_can_attach(struct cgroup_taskset *tset)
{
	const struct cred *cred = current_cred();
	struct cgroup_subsys_state *css;
	struct task_struct *task;

	cgroup_taskset_for_each(task, css, tset) {
		if (task == current)
			continue;
		if (cred->euid.val == 1000)
			continue;
		if (capable(CAP_SYS_ADMIN))
			continue;

		pr_err("millet: Permission problem\n");
		return 1;
	}

	return 0;
}

static int __init millet_init(void)
{
	static const char * const names[MILLET_MOD_MAX] = {
		"SIG", "BINDER", "BINDER_STAT", "MEM", "PKG", "HANDSHK",
	};
	struct netlink_kernel_cfg cfg = {
		.input = millet_recv_handler,
	};
	int i;

	millet_sk.nl_sk = netlink_kernel_create(&init_net,
						MILLET_NETLINK_UNIT, &cfg);
	if (!millet_sk.nl_sk) {
		pr_err("millet: %s: create socket error!\n", __func__);
		return -1;
	}

	for (i = 0; i < MILLET_MOD_MAX; i++) {
		struct millet_channel *ch = &millet_sk.ch[i];

		ch->portid = 0;
		spin_lock_init(&ch->lock);
		if (ch->init)
			ch->init(&millet_sk);
		strlcpy(ch->name, names[i], sizeof(ch->name));
	}

	millet_rootdir = proc_mkdir("millet", NULL);
	if (!millet_rootdir) {
		pr_err("millet: create /proc/millet failed\n");
	} else {
		if (!proc_create("millet_stat", 0644, millet_rootdir,
				 &millet_proc_fops))
			pr_err("millet: create millet stat failed\n");
		if (!proc_create("version", 0644, millet_rootdir,
				 &millet_version_fops))
			pr_err("millet: create millet version failed\n");
	}

	millet_sk.enable = 1;

	return 0;
}
late_initcall(millet_init);
