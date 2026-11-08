// SPDX-License-Identifier: GPL-2.0
// SPI driven IR LED device driver
//
// Copyright (c) 2016 Samsung Electronics Co., Ltd.
// Copyright (C) 2021 XiaoMi, Inc.
// Copyright (c) Andi Shyti <andi@etezian.org>
//
// Exposes the standard rc-core transmitter (/dev/lirc0) and the
// XiaoMi/MIUI raw transmitter (/dev/ir_spi).

#include <linux/compat.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/limits.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>
#include <media/rc-core.h>
#include <uapi/linux/lirc.h>

#include <asm/hwconf_manager.h>

#define IR_SPI_DRIVER_NAME		"ir-spi"

#define IR_SPI_DEFAULT_FREQUENCY	38000
#define IR_SPI_MAX_BUFSIZE		32768

/* XiaoMi/MIUI raw interface */
#define IR_SPI_RAW_DRIVER_NAME		"ir_spi"
#define IR_SPI_RAW_DEFAULT_FREQUENCY	1920000
#define IR_SPI_RAW_BITS_PER_WORD	32
#define IR_SPI_RAW_BUFSIZE		150000

struct ir_spi_data {
	/* rc-core path */
	u32 freq;
	bool negated;

	u16 tx_buf[IR_SPI_MAX_BUFSIZE];
	u16 pulse;
	u16 space;

	struct rc_dev *rc;

	/* XiaoMi/MIUI raw path */
	struct miscdevice misc;
	struct mutex raw_lock;
	u8 *raw_buf;
	u32 raw_size;
	u32 raw_len;
	u16 nusers;

	/* Shared hardware */
	struct spi_device *spi;
	struct regulator *regulator;
	struct mutex tx_lock;
};

/*
 * rc-core path
 */

static int ir_spi_tx(struct rc_dev *dev,
		     unsigned int *buffer, unsigned int count)
{
	int i;
	int ret;
	unsigned int len = 0;
	struct ir_spi_data *idata = dev->priv;
	struct spi_transfer xfer;

	/* convert the pulse/space signal to raw binary signal */
	for (i = 0; i < count; i++) {
		unsigned int periods;
		int j;
		u16 val;

		periods = DIV_ROUND_CLOSEST(buffer[i] * idata->freq, 1000000);

		if (len + periods >= IR_SPI_MAX_BUFSIZE)
			return -EINVAL;

		/*
		 * the first value in buffer is a pulse, so that 0, 2, 4, ...
		 * contain a pulse duration. On the contrary, 1, 3, 5, ...
		 * contain a space duration.
		 */
		val = (i % 2) ? idata->space : idata->pulse;
		for (j = 0; j < periods; j++)
			idata->tx_buf[len++] = val;
	}

	memset(&xfer, 0, sizeof(xfer));

	xfer.speed_hz = idata->freq * 16;
	xfer.len = len * sizeof(*idata->tx_buf);
	xfer.tx_buf = idata->tx_buf;

	mutex_lock(&idata->tx_lock);

	ret = regulator_enable(idata->regulator);
	if (ret) {
		mutex_unlock(&idata->tx_lock);
		return ret;
	}

	ret = spi_sync_transfer(idata->spi, &xfer, 1);
	if (ret)
		dev_err(&idata->spi->dev, "unable to deliver the signal\n");

	regulator_disable(idata->regulator);

	mutex_unlock(&idata->tx_lock);

	return ret ? ret : count;
}

static int ir_spi_set_tx_carrier(struct rc_dev *dev, u32 carrier)
{
	struct ir_spi_data *idata = dev->priv;

	if (!carrier)
		return -EINVAL;

	idata->freq = carrier;

	return 0;
}

static int ir_spi_set_duty_cycle(struct rc_dev *dev, u32 duty_cycle)
{
	struct ir_spi_data *idata = dev->priv;
	int bits = (duty_cycle * 15) / 100;

	idata->pulse = GENMASK(bits, 0);

	if (idata->negated) {
		idata->pulse = ~idata->pulse;
		idata->space = 0xffff;
	} else {
		idata->space = 0;
	}

	return 0;
}

/*
 * XiaoMi/MIUI raw path
 *
 * raw_len keeps the expected write length, raw_buf/raw_size the backing
 * buffer. The write/ioctl semantics match the vendor driver.
 */

static ssize_t ir_spi_raw_write(struct file *file,
				const char __user *buffer,
				size_t length, loff_t *offset)
{
	struct ir_spi_data *idata = file->private_data;
	bool please_free = false;
	int ret = 0;

	mutex_lock(&idata->raw_lock);

	if (idata->raw_len && (idata->raw_len != length)) {
		ret = -EINVAL;
		goto out_unlock;
	}

	if (!idata->raw_len) {
		u8 *new;

		/* keep the buffer non-NULL for zero-length transfers */
		new = kmalloc(length ? length : 1, GFP_DMA);
		if (!new) {
			ret = -ENOMEM;
			goto out_unlock;
		}

		/* replace any previous buffer */
		kfree(idata->raw_buf);
		idata->raw_buf = new;
		idata->raw_size = length;
		idata->raw_len = length;
		please_free = true;
	}

	if (copy_from_user(idata->raw_buf, buffer, length)) {
		ret = -EFAULT;
		goto out_free;
	}

	{
		struct spi_transfer xfer = {
			.tx_buf		= idata->raw_buf,
			.len		= (unsigned int)length,
			.speed_hz	= IR_SPI_RAW_DEFAULT_FREQUENCY,
			.bits_per_word	= IR_SPI_RAW_BITS_PER_WORD,
		};

		mutex_lock(&idata->tx_lock);

		ret = regulator_enable(idata->regulator);
		if (!ret) {
			ret = spi_sync_transfer(idata->spi, &xfer, 1);
			regulator_disable(idata->regulator);
		}

		mutex_unlock(&idata->tx_lock);
	}

	if (ret)
		dev_err(&idata->spi->dev, "unable to deliver the signal\n");

out_free:
	if (please_free) {
		kfree(idata->raw_buf);
		idata->raw_buf = NULL;
		idata->raw_len = 0;
	}

out_unlock:
	mutex_unlock(&idata->raw_lock);

	return ret ? ret : length;
}

static int ir_spi_raw_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct ir_spi_data *idata = container_of(misc, struct ir_spi_data, misc);

	if (unlikely(idata->nusers >= SHRT_MAX)) {
		dev_err(&idata->spi->dev, "device busy\n");
		return -EBUSY;
	}

	mutex_lock(&idata->raw_lock);
	idata->nusers++;
	mutex_unlock(&idata->raw_lock);

	file->private_data = idata;

	return 0;
}

static int ir_spi_raw_release(struct inode *inode, struct file *file)
{
	struct ir_spi_data *idata = file->private_data;

	mutex_lock(&idata->raw_lock);

	if (idata->nusers)
		idata->nusers--;

	/* drop the expected length when the last user closes */
	if (!idata->nusers)
		idata->raw_len = 0;

	mutex_unlock(&idata->raw_lock);

	return 0;
}

static long ir_spi_raw_ioctl(struct file *file, unsigned int cmd,
			     unsigned long arg)
{
	struct ir_spi_data *idata = file->private_data;
	u32 p;
	int ret;

	if (cmd != LIRC_SET_SEND_MODE)
		return -EINVAL;

	ret = get_user(p, (__u32 __user *)arg);
	if (ret)
		return ret;

	mutex_lock(&idata->raw_lock);

	/* same length as the current value */
	if (idata->raw_len == p) {
		ret = 0;
		goto out_unlock;
	}

	/* multiple users must agree on the length */
	if (idata->nusers > 1) {
		ret = -EPERM;
		goto out_unlock;
	}

	/* reallocate to the requested length; 0 frees the buffer */
	if (idata->raw_len) {
		if (!p) {
			kfree(idata->raw_buf);
			idata->raw_buf = NULL;
			idata->raw_size = 0;
		} else {
			void *new = krealloc(idata->raw_buf, p, GFP_DMA);

			if (!new) {
				ret = -ENOMEM;
				goto out_unlock;
			}

			idata->raw_buf = new;
			idata->raw_size = p;
		}
	} else if (p > idata->raw_size || idata->raw_buf == NULL) {
		void *new;

		kfree(idata->raw_buf);
		idata->raw_buf = NULL;

		new = kmalloc(p ? p : 1, GFP_DMA);
		if (!new) {
			ret = -ENOMEM;
			goto out_unlock;
		}

		idata->raw_buf = new;
		idata->raw_size = p;
	}

	idata->raw_len = p;
	ret = 0;

out_unlock:
	mutex_unlock(&idata->raw_lock);

	return ret;
}

static const struct file_operations ir_spi_raw_fops = {
	.owner		= THIS_MODULE,
	.write		= ir_spi_raw_write,
	.open		= ir_spi_raw_open,
	.release	= ir_spi_raw_release,
	.llseek		= noop_llseek,
	.unlocked_ioctl	= ir_spi_raw_ioctl,
	.compat_ioctl	= ir_spi_raw_ioctl,
};

static int ir_spi_probe(struct spi_device *spi)
{
	int ret;
	u8 dc;
	u8 *buffer;
	struct ir_spi_data *idata;

	idata = devm_kzalloc(&spi->dev, sizeof(*idata), GFP_KERNEL);
	if (!idata)
		return -ENOMEM;

	mutex_init(&idata->raw_lock);
	mutex_init(&idata->tx_lock);

	idata->spi = spi;
	spi_set_drvdata(spi, idata);

	idata->regulator = devm_regulator_get(&spi->dev, "irda_regulator");
	if (IS_ERR(idata->regulator))
		return PTR_ERR(idata->regulator);

	/* rc-core device (/dev/lirc0) */
	idata->rc = devm_rc_allocate_device(&spi->dev, RC_DRIVER_IR_RAW_TX);
	if (!idata->rc)
		return -ENOMEM;

	idata->rc->tx_ir           = ir_spi_tx;
	idata->rc->s_tx_carrier    = ir_spi_set_tx_carrier;
	idata->rc->s_tx_duty_cycle = ir_spi_set_duty_cycle;
	idata->rc->device_name	   = "IR SPI";
	idata->rc->driver_name     = IR_SPI_DRIVER_NAME;
	idata->rc->priv            = idata;
	idata->spi                 = spi;

	idata->negated = of_property_read_bool(spi->dev.of_node,
							"led-active-low");
	ret = of_property_read_u8(spi->dev.of_node, "duty-cycle", &dc);
	if (ret)
		dc = 50;

	/* ir_spi_set_duty_cycle cannot fail,
	 * it returns int to be compatible with the
	 * rc->s_tx_duty_cycle function
	 */
	ir_spi_set_duty_cycle(idata->rc, dc);

	idata->freq = IR_SPI_DEFAULT_FREQUENCY;

	ret = devm_rc_register_device(&spi->dev, idata->rc);
	if (ret)
		return ret;

	/* XiaoMi/MIUI raw interface (/dev/ir_spi) */
	buffer = kmalloc(IR_SPI_RAW_BUFSIZE, GFP_DMA);
	if (!buffer)
		return -ENOMEM;

	idata->raw_buf = buffer;
	idata->raw_size = IR_SPI_RAW_BUFSIZE;

	idata->misc.minor    = MISC_DYNAMIC_MINOR;
	idata->misc.name     = IR_SPI_RAW_DRIVER_NAME;
	idata->misc.nodename = IR_SPI_RAW_DRIVER_NAME;
	idata->misc.fops     = &ir_spi_raw_fops;
	idata->misc.mode     = 0666;

	ret = misc_register(&idata->misc);
	if (ret) {
		dev_err(&spi->dev, "failed to register %s: %d\n",
			IR_SPI_RAW_DRIVER_NAME, ret);
		kfree(idata->raw_buf);
		idata->raw_buf = NULL;
		return ret;
	}

	register_hw_component_info("ir");
	add_hw_component_info("ir", "type", "ir_spi");

	return 0;
}

static int ir_spi_remove(struct spi_device *spi)
{
	struct ir_spi_data *idata = spi_get_drvdata(spi);

	if (!idata)
		return 0;

	unregister_hw_component_info("ir");
	misc_deregister(&idata->misc);

	kfree(idata->raw_buf);
	idata->raw_buf = NULL;

	return 0;
}

static const struct of_device_id ir_spi_of_match[] = {
	{ .compatible = "ir-spi-led" },
	{ .compatible = "ir-spi-xiaomi" },
	{ .compatible = "ir-spi" },
	{},
};
MODULE_DEVICE_TABLE(of, ir_spi_of_match);

static struct spi_driver ir_spi_driver = {
	.probe = ir_spi_probe,
	.remove = ir_spi_remove,
	.driver = {
		.name = IR_SPI_DRIVER_NAME,
		.of_match_table = ir_spi_of_match,
	},
};

module_spi_driver(ir_spi_driver);

MODULE_AUTHOR("Andi Shyti <andi@etezian.org>");
MODULE_DESCRIPTION("SPI IR LED");
MODULE_LICENSE("GPL v2");
