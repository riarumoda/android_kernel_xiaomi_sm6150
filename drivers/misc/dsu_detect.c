// SPDX-License-Identifier: GPL-2.0
/*
 * DSU (Dynamic System Update) detector.
 *
 * A DSU boot reuses the host kernel, so there is no bootloader flag to read.
 * First-stage init maps the DSU images with device-mapper names ending in
 * "_gsi".  The device-mapper ioctl path reports every created mapping here,
 * and the boot is only treated as a DSU boot when the root filesystem is
 * backed by one of them.  Temporary mappings used while installing a DSU
 * therefore do not mark the running system as a DSU.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/dsu_detect.h>

#define DSU_NAME_MAX		64
#define DSU_MAX_MAPPINGS	32
#define DSU_SUFFIX		"_gsi"

/* -1: auto, 0: off, 1: force on */
static int dsu_forced = -1;

static bool dsu_active;
static bool dsu_logged;

static DEFINE_MUTEX(dsu_lock);
static struct dsu_mapping {
	char name[DSU_NAME_MAX];
	dev_t dev;
} dsu_seen[DSU_MAX_MAPPINGS];
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

static bool dsu_root_on_gsi(void)
{
	struct block_device *bdev;
	struct path path;
	unsigned int i;
	bool found = false;

	if (kern_path("/", LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &path))
		return false;

	bdev = path.dentry->d_sb->s_bdev;
	if (!bdev)
		goto out;

	mutex_lock(&dsu_lock);
	for (i = 0; i < dsu_seen_count; i++) {
		if (dsu_seen[i].dev == bdev->bd_dev) {
			found = true;
			break;
		}
	}
	mutex_unlock(&dsu_lock);

out:
	path_put(&path);
	return found;
}

bool dsu_detect_active(void)
{
	if (READ_ONCE(dsu_active))
		return true;

	if (dsu_forced == 1) {
		dsu_mark_active();
		return true;
	}

	if (dsu_forced == 0)
		return false;

	if (dsu_root_on_gsi()) {
		dsu_mark_active();
		return true;
	}

	return false;
}
EXPORT_SYMBOL_GPL(dsu_detect_active);

void dsu_detect_note_dm_name(const char *name, dev_t dev)
{
	size_t len;
	unsigned int i;

	if (!name || dsu_forced == 0 || READ_ONCE(dsu_active))
		return;

	len = strlen(name);
	if (len < sizeof(DSU_SUFFIX) - 1 ||
	    strcmp(name + len - (sizeof(DSU_SUFFIX) - 1), DSU_SUFFIX))
		return;

	mutex_lock(&dsu_lock);

	for (i = 0; i < dsu_seen_count; i++) {
		if (dsu_seen[i].dev == dev)
			goto out;
	}

	if (dsu_seen_count >= DSU_MAX_MAPPINGS)
		goto out;

	strscpy(dsu_seen[dsu_seen_count].name, name, DSU_NAME_MAX);
	dsu_seen[dsu_seen_count].dev = dev;
	dsu_seen_count++;
out:
	mutex_unlock(&dsu_lock);
}
EXPORT_SYMBOL_GPL(dsu_detect_note_dm_name);

static int dsu_detect_proc_show(struct seq_file *m, void *v)
{
	unsigned int i;

	seq_printf(m, "active: %d\n", dsu_detect_active() ? 1 : 0);
	seq_printf(m, "forced: %d\n", dsu_forced);

	mutex_lock(&dsu_lock);
	for (i = 0; i < dsu_seen_count; i++)
		seq_printf(m, "mapping: %s %u:%u\n", dsu_seen[i].name,
			   MAJOR(dsu_seen[i].dev), MINOR(dsu_seen[i].dev));
	mutex_unlock(&dsu_lock);

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
