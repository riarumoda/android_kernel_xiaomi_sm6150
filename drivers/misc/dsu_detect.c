// SPDX-License-Identifier: GPL-2.0
/*
 * DSU (Dynamic System Update) detector.
 *
 * A DSU boot reuses the host kernel, so there is no bootloader flag to read.
 * First-stage init maps the DSU images with device-mapper names ending in
 * "_gsi".  The device-mapper ioctl path reports every created name here, so
 * the result does not depend on sysfs access from a kernel thread.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/dsu_detect.h>

#define DSU_NAME_MAX		64
#define DSU_MAX_MAPPINGS	32

/* -1: auto, 0: off, 1: force on */
static int dsu_forced = -1;

static bool dsu_active;
static bool dsu_logged;

static DEFINE_MUTEX(dsu_lock);
static char dsu_seen[DSU_MAX_MAPPINGS][DSU_NAME_MAX];
static unsigned int dsu_seen_count;

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

static void dsu_mark_active(void)
{
	if (READ_ONCE(dsu_active))
		return;

	WRITE_ONCE(dsu_active, true);
	if (!READ_ONCE(dsu_logged)) {
		WRITE_ONCE(dsu_logged, true);
		pr_info("DSU detected\n");
	}
}

bool dsu_detect_active(void)
{
	if (READ_ONCE(dsu_active))
		return true;

	if (dsu_forced == 1) {
		dsu_mark_active();
		return true;
	}

	return false;
}
EXPORT_SYMBOL_GPL(dsu_detect_active);

void dsu_detect_note_dm_name(const char *name)
{
	unsigned int i;

	if (!name || dsu_forced == 0 || READ_ONCE(dsu_active))
		return;

	if (!strstr(name, "_gsi"))
		return;

	mutex_lock(&dsu_lock);

	for (i = 0; i < dsu_seen_count; i++) {
		if (!strcmp(dsu_seen[i], name))
			goto out;
	}

	if (dsu_seen_count >= DSU_MAX_MAPPINGS)
		goto out;

	strscpy(dsu_seen[dsu_seen_count], name, DSU_NAME_MAX);
	dsu_seen_count++;

	/*
	 * A live DSU maps at least the system image and its userdata image.
	 * A single mapping is not enough to tell it apart from an install.
	 */
	if (dsu_seen_count >= 2)
		dsu_mark_active();
out:
	mutex_unlock(&dsu_lock);
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

	pr_info("DSU detector ready (forced=%d)\n", dsu_forced);

	return 0;
}
late_initcall(dsu_detect_init);
