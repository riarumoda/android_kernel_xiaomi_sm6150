// SPDX-License-Identifier: GPL-2.0
/*
 * Panel physical size override.
 *
 * Display drivers call panel_override_apply() when filling connector
 * information.  Zero keeps the size supplied by the device tree.
 */

#include <linux/init.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/panel_override.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#define PANEL_MAX_MM	10000

static unsigned int panel_width_mm;
static unsigned int panel_height_mm;
static bool panel_applied;

static int panel_param_set(const char *val, const struct kernel_param *kp)
{
	unsigned int v;
	int ret;

	ret = kstrtouint(val, 0, &v);
	if (ret)
		return ret;

	if (v > PANEL_MAX_MM)
		return -EINVAL;

	WRITE_ONCE(*(unsigned int *)kp->arg, v);
	WRITE_ONCE(panel_applied, false);

	return 0;
}

static const struct kernel_param_ops panel_param_ops = {
	.set	= panel_param_set,
	.get	= param_get_uint,
};

module_param_cb(width_mm, &panel_param_ops, &panel_width_mm, 0644);
MODULE_PARM_DESC(width_mm, "Panel width in millimeters (0 = use device tree)");

module_param_cb(height_mm, &panel_param_ops, &panel_height_mm, 0644);
MODULE_PARM_DESC(height_mm, "Panel height in millimeters (0 = use device tree)");

int panel_override_set(u32 width_mm, u32 height_mm)
{
	if (width_mm > PANEL_MAX_MM || height_mm > PANEL_MAX_MM)
		return -EINVAL;

	WRITE_ONCE(panel_width_mm, width_mm);
	WRITE_ONCE(panel_height_mm, height_mm);
	WRITE_ONCE(panel_applied, false);

	return 0;
}
EXPORT_SYMBOL_GPL(panel_override_set);

void panel_override_apply(u32 *width_mm, u32 *height_mm)
{
	u32 w = READ_ONCE(panel_width_mm);
	u32 h = READ_ONCE(panel_height_mm);

	if (!width_mm || !height_mm || (!w && !h))
		return;

	if (w)
		*width_mm = w;
	if (h)
		*height_mm = h;

	if (!READ_ONCE(panel_applied)) {
		WRITE_ONCE(panel_applied, true);
		pr_info("panel override: %ux%u mm\n", *width_mm, *height_mm);
	}
}
EXPORT_SYMBOL_GPL(panel_override_apply);

static int panel_override_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "width_mm %u\n", READ_ONCE(panel_width_mm));
	seq_printf(m, "height_mm %u\n", READ_ONCE(panel_height_mm));

	return 0;
}

static ssize_t panel_override_proc_write(struct file *file,
					 const char __user *ubuf,
					 size_t count, loff_t *ppos)
{
	char buf[32];
	unsigned int w, h;

	if (!count || count >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;

	buf[count] = '\0';
	strim(buf);

	if (!strcmp(buf, "0") || !strcmp(buf, "off")) {
		w = 0;
		h = 0;
	} else if (sscanf(buf, "%u %u", &w, &h) != 2 &&
		   sscanf(buf, "%ux%u", &w, &h) != 2) {
		return -EINVAL;
	}

	if (w > PANEL_MAX_MM || h > PANEL_MAX_MM)
		return -EINVAL;

	if (panel_override_set(w, h))
		return -EINVAL;

	pr_info("panel override set to %ux%u mm\n", w, h);

	return count;
}

static int panel_override_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, panel_override_proc_show, NULL);
}

static const struct file_operations panel_override_proc_fops = {
	.open		= panel_override_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.write		= panel_override_proc_write,
	.release	= single_release,
};

static int __init panel_override_init(void)
{
	proc_create("panel_override", 0644, NULL, &panel_override_proc_fops);

	pr_info("panel override ready (width=%u mm, height=%u mm)\n",
		READ_ONCE(panel_width_mm), READ_ONCE(panel_height_mm));

	return 0;
}
module_init(panel_override_init);

MODULE_DESCRIPTION("Panel physical size override");
MODULE_LICENSE("GPL");
