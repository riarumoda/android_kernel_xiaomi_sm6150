// SPDX-License-Identifier: GPL-2.0
/*
 * DSU (Dynamic System Update) detector.
 *
 * A DSU boot reuses the host kernel, so there is no bootloader flag to read.
 * First-stage init maps the DSU images with device-mapper names ending in
 * "_gsi".  This driver watches those mappings on its own and publishes the
 * result through /proc/dsu_detect.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/dsu_detect.h>

#define DSU_DM_MAX		64
#define DSU_SCAN_INTERVAL_MS	1000
#define DSU_SCAN_TIMEOUT_MS	60000

/* -1: auto, 0: off, 1: force on */
static int dsu_forced = -1;

static bool dsu_active;
static bool dsu_logged;
static unsigned long dsu_start;

static void dsu_scan_work(struct work_struct *work);
static DECLARE_DELAYED_WORK(dsu_work, dsu_scan_work);

static int __init dsu_detect_setup(char *str)
{
	if (!str)
		return 1;

	if (!strcmp(str, "1") || !strcmp(str, "on") || !strcmp(str, "yes"))
		dsu_forced = 1;
	else
		dsu_forced = 0;

	return 1;
}
__setup("dsu_detect=", dsu_detect_setup);

struct dsu_dir_ctx {
	struct dir_context ctx;
	unsigned int count;
};

static int dsu_dir_actor(struct dir_context *ctx, const char *name, int namlen,
			 loff_t offset, u64 ino, unsigned int d_type)
{
	struct dsu_dir_ctx *dctx = container_of(ctx, struct dsu_dir_ctx, ctx);

	if ((namlen == 1 && name[0] == '.') ||
	    (namlen == 2 && name[0] == '.' && name[1] == '.'))
		return 0;

	dctx->count++;
	return 0;
}

static bool dsu_dm_has_holders(int index)
{
	struct dsu_dir_ctx dctx = {
		.ctx.actor = dsu_dir_actor,
	};
	struct file *filp;
	char path[64];

	snprintf(path, sizeof(path), "/sys/block/dm-%d/holders", index);

	filp = filp_open(path, O_RDONLY | O_DIRECTORY, 0);
	if (IS_ERR(filp))
		return false;

	iterate_dir(filp, &dctx.ctx);
	filp_close(filp, NULL);

	return dctx.count > 0;
}

static bool dsu_scan_dm(void)
{
	unsigned int gsi_count = 0;
	bool in_use = false;
	int i;

	for (i = 0; i < DSU_DM_MAX; i++) {
		char path[64];
		char name[64];
		struct file *filp;
		ssize_t len;
		loff_t pos = 0;

		snprintf(path, sizeof(path), "/sys/block/dm-%d/dm/name", i);

		filp = filp_open(path, O_RDONLY, 0);
		if (IS_ERR(filp))
			continue;

		len = kernel_read(filp, name, sizeof(name) - 1, &pos);
		filp_close(filp, NULL);

		if (len <= 0)
			continue;

		name[len] = '\0';
		strim(name);

		if (!strstr(name, "_gsi"))
			continue;

		gsi_count++;
		if (dsu_dm_has_holders(i))
			in_use = true;
	}

	/*
	 * A live DSU maps at least the system image and its userdata image.
	 * A single temporary mapping (DSU installation) has no holder.
	 */
	return in_use || gsi_count >= 2;
}

static void dsu_scan_work(struct work_struct *work)
{
	if (dsu_forced == 0 || READ_ONCE(dsu_active))
		return;

	if (dsu_forced == 1 || dsu_scan_dm()) {
		WRITE_ONCE(dsu_active, true);
		if (!READ_ONCE(dsu_logged)) {
			WRITE_ONCE(dsu_logged, true);
			pr_info("DSU detected\n");
		}
		return;
	}

	if (time_before(jiffies, dsu_start + msecs_to_jiffies(DSU_SCAN_TIMEOUT_MS)))
		schedule_delayed_work(&dsu_work,
				      msecs_to_jiffies(DSU_SCAN_INTERVAL_MS));
}

bool dsu_detect_active(void)
{
	if (READ_ONCE(dsu_active))
		return true;

	if (dsu_forced == 1) {
		WRITE_ONCE(dsu_active, true);
		return true;
	}

	return false;
}
EXPORT_SYMBOL_GPL(dsu_detect_active);

void dsu_detect_note_dm_name(const char *name)
{
	if (!name || dsu_forced == 0 || READ_ONCE(dsu_active))
		return;

	if (strstr(name, "_gsi"))
		schedule_delayed_work(&dsu_work,
				      msecs_to_jiffies(DSU_SCAN_INTERVAL_MS));
}
EXPORT_SYMBOL_GPL(dsu_detect_note_dm_name);

static int dsu_detect_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "active: %d\n", dsu_detect_active() ? 1 : 0);
	seq_printf(m, "forced: %d\n", dsu_forced);

	return 0;
}

static int dsu_detect_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, dsu_detect_proc_show, NULL);
}

static const struct file_operations dsu_detect_proc_fops = {
	.open		= dsu_detect_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init dsu_detect_init(void)
{
	proc_create("dsu_detect", 0444, NULL, &dsu_detect_proc_fops);

	dsu_start = jiffies;
	schedule_delayed_work(&dsu_work, 0);

	pr_info("DSU detector ready (forced=%d)\n", dsu_forced);

	return 0;
}
late_initcall(dsu_detect_init);
