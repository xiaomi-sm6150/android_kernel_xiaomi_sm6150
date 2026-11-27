// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/bitrev.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include "dpin_cci_util.h"

/* CCI configuration — sourced from Device Tree at probe time. */
#define CCI_DEV_INDEX_DEFAULT  0
#define CCI_MASTER_ID_DEFAULT  0
#define SLAVE_ADDR_DEFAULT     0x86

struct cci_util_dev {
	struct platform_device *ppdev;
	struct dpin_cci_util_sensor_client client;
	bool configured;
	int cci_dev_index;
	int cci_master_id;
	int slave_addr;
};

/**
 * struct cci_util_handle - opaque handle handed to clients.
 *
 * Wraps a pointer back to the owning cci_util_dev so that all exported
 * functions can reach private driver state without touching any global
 * variable.  The struct is forward-declared in the public header; its
 * layout is private to this file.
 */
struct cci_util_handle {
	struct cci_util_dev *dev;
};

static struct cci_util_dev *g_cci_util;
static struct device *g_cci_dev;
static DEFINE_MUTEX(g_cci_util_lock);

/**
 * cci_util_lt7911_get_device - obtain a handle to the CCI-util device.
 *
 * Acquires g_cci_util_lock to safely read g_cci_util.  Returns NULL when
 * the driver has not yet probed or has already been removed, giving the
 * caller a natural "not ready" signal before any other API is called.
 * The caller must release the handle with cci_util_lt7911_put_device().
 */
struct cci_util_handle *cci_util_lt7911_get_device(void)
{
	struct cci_util_handle *handle;

	mutex_lock(&g_cci_util_lock);
	if (!g_cci_util) {
		mutex_unlock(&g_cci_util_lock);
		return NULL;
	}

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (handle)
		handle->dev = g_cci_util;
	mutex_unlock(&g_cci_util_lock);
	return handle;
}
EXPORT_SYMBOL_GPL(cci_util_lt7911_get_device);

/**
 * cci_util_lt7911_put_device - release a handle obtained from get_device.
 * @handle: handle to release (NULL is silently ignored)
 */
void cci_util_lt7911_put_device(struct cci_util_handle *handle)
{
	kfree(handle);
}
EXPORT_SYMBOL_GPL(cci_util_lt7911_put_device);

static struct dpin_cci_util_i2c_reg_array i2c_enable[] = {
	{ .reg_addr = 0xff, .reg_data = 0xe0, .delay = 0x00, .data_mask = 0x00},
	{ .reg_addr = 0xee, .reg_data = 0x01, .delay = 0x00, .data_mask = 0x00},
	{ .reg_addr = 0xff, .reg_data = 0xe1, .delay = 0x00, .data_mask = 0x00},
};

static struct dpin_cci_util_i2c_reg_array i2c_disable[] = {
	{ .reg_addr = 0xff, .reg_data = 0xe0, .delay = 0x00, .data_mask = 0x00},
	{ .reg_addr = 0xee, .reg_data = 0x00, .delay = 0x00, .data_mask = 0x00},
};

/*
 * LT7911 register map
 */
#define LT7911UXC_INTERRUPT_TYPE_REG    0x84
#define LT7911_INFORMATION_FROM_84_NUM  17
#define LT7911UXC_INTERRUPT_TYPE_IDX    0
#define LT7911UXC_PCLOCK_3_IDX          1
#define LT7911UXC_PCLOCK_2_IDX          2
#define LT7911UXC_PCLOCK_1_IDX          3
#define LT7911UXC_HTOTAL_MSB_IDX        4
#define LT7911UXC_HTOTAL_LSB_IDX        5
#define LT7911UXC_VTOTAL_MSB_IDX        6
#define LT7911UXC_VTOTAL_LSB_IDX        7
#define LT7911UXC_HACTIVE_MSB_IDX       8
#define LT7911UXC_HACTIVE_LSB_IDX       9
#define LT7911UXC_VACTIVE_MSB_IDX       10
#define LT7911UXC_VACTIVE_LSB_IDX       11
#define LT7911UXC_AUDIO_FREQ_MSB_IDX    12
#define LT7911UXC_AUDIO_FREQ_LSB_IDX    13

#define LT7911UXC_PORT_NUM_REG          0xA0
#define LT7911UXC_MIPI_FORMAT_IDX       1
#define LT7911UXC_AUDIO_CHANNEL_IDX     2
#define LT7911_INFORMATION_FROM_A0_NUM  3

/*
 * LT7911 firmware upgrade — CRC helpers
 */
struct lt7911_crc_info {
	uint8_t  width;
	uint32_t poly;
	uint32_t crc_init;
	uint32_t xor_out;
	bool     ref_in;
	bool     ref_out;
};

/*
 * LT7911 firmware upgrade register / constant definitions
 */
#define LT7911UXC_VERSION_REG0          0x80
#define LT7911UXC_VERSION_REG1          0x82
#define LT7911UXC_HDCP_VERSION_REG      0x95
#define LT7911UXC_CC_SWITCH_REG         0xb7
#define LT7911UXC_SWAP_APPLY_REG        0xb8
#define LT7911UXC_DP_TRANING_REG        0xa4
#define LT7911_HDCPKEY_ADDR             0x050000
#define LT7911_HDCPKEY_SIZE             32
#define LT7911_BYTESIZE_PER_PAGE        32
#define LT7911_READNUM_SET_COMMAND      0x5f
#define LT7911UXC_FW_AREA_SIZE          (64 * 1024)

static int dpin_cci_util_cmd(struct dpin_cci_util_sensor_client *client,
			 enum dpin_cci_util_cmd_type cmd)
{
	struct dpin_cci_util_ctrl ctrl;

	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.cmd      = cmd;
	ctrl.cci_info = client;

	return v4l2_subdev_call(client->cci_subdev, core, ioctl,
				DPIN_CCI_UTIL_VIDIOC_MSM_CCI_CFG, &ctrl);
}

static int dpin_cci_util_write(struct dpin_cci_util_sensor_client *client,
			  struct dpin_cci_util_i2c_reg_setting *setting)
{
	int rc;
	struct dpin_cci_util_ctrl ctrl;

	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.cmd                               = DPIN_CCI_UTIL_MSM_CCI_I2C_WRITE;
	ctrl.cci_info                          = client;
	ctrl.cfg.cci_i2c_write_cfg.reg_setting = setting->reg_setting;
	ctrl.cfg.cci_i2c_write_cfg.size        = setting->size;
	ctrl.cfg.cci_i2c_write_cfg.addr_type   = setting->addr_type;
	ctrl.cfg.cci_i2c_write_cfg.data_type   = setting->data_type;
	ctrl.cfg.cci_i2c_write_cfg.delay       = setting->delay;

	rc = v4l2_subdev_call(client->cci_subdev, core, ioctl,
			      DPIN_CCI_UTIL_VIDIOC_MSM_CCI_CFG, &ctrl);
	if (rc < 0)
		return rc;
	return ctrl.status;
}

/**
 * dpin_cci_util_read - read @num_bytes bytes from @reg_addr into @out_buf.
 * @client:    populated CCI client (sid, master, subdev already set)
 * @reg_addr:  register address to read from
 * @addr_type: address width (DPIN_CCI_UTIL_I2C_TYPE_BYTE / _WORD)
 * @data_type: data width  (DPIN_CCI_UTIL_I2C_TYPE_BYTE / _WORD)
 * @out_buf:   caller-allocated u32 to receive the assembled value
 * @num_bytes: number of bytes to read
 *
 * Return: 0 on success, negative errno on failure.
 */
static int dpin_cci_util_read(struct dpin_cci_util_sensor_client *client,
			 u8 reg_addr,
			 enum dpin_cci_util_i2c_type addr_type,
			 enum dpin_cci_util_i2c_type data_type,
			 u32 *out_buf,
			 u16 num_bytes)
{
	int rc;
	struct dpin_cci_util_ctrl ctrl;
	unsigned char buf[4] = { 0 };

	if (!out_buf || num_bytes == 0 || num_bytes > sizeof(buf))
		return -EINVAL;

	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.cmd                              = DPIN_CCI_UTIL_MSM_CCI_I2C_READ;
	ctrl.cci_info                         = client;
	ctrl.cfg.cci_i2c_read_cfg.addr        = reg_addr;
	ctrl.cfg.cci_i2c_read_cfg.addr_type   = (u16)addr_type;
	ctrl.cfg.cci_i2c_read_cfg.data_type   = (u16)data_type;
	ctrl.cfg.cci_i2c_read_cfg.num_byte    = num_bytes;
	ctrl.cfg.cci_i2c_read_cfg.data        = buf;

	rc = v4l2_subdev_call(client->cci_subdev, core, ioctl,
			      DPIN_CCI_UTIL_VIDIOC_MSM_CCI_CFG, &ctrl);
	if (rc < 0)
		return rc;
	if (ctrl.status < 0)
		return ctrl.status;
	if (data_type == DPIN_CCI_UTIL_I2C_TYPE_BYTE)
		*out_buf = buf[0];
	else if (data_type == DPIN_CCI_UTIL_I2C_TYPE_WORD)
		*out_buf = (u32)buf[0] << 8 | buf[1];
	else if (data_type == DPIN_CCI_UTIL_I2C_TYPE_3B)
		*out_buf = (u32)buf[0] << 16 | (u32)buf[1] << 8 | buf[2];
	else
		*out_buf = (u32)buf[0] << 24 | (u32)buf[1] << 16 |
			   (u32)buf[2] << 8  | buf[3];
	dev_dbg(client->cci_subdev->dev, "%s, reg_addr:%x, out_buf:%x\n",
		__func__, reg_addr, *out_buf);
	return ctrl.status;
}

static int cci_configure(struct cci_util_dev *dpbdev)
{
	int rc;

	dpbdev->client.cci_subdev = platform_get_drvdata(dpbdev->ppdev);
	if (!dpbdev->client.cci_subdev) {
		dev_err(&dpbdev->ppdev->dev,
			"[dpin_cci_util] cam_cci_get_subdev(%d) returned NULL\n",
			dpbdev->cci_dev_index);
		return -ENODEV;
	}

	dpbdev->client.cci_device     = dpbdev->cci_dev_index;
	dpbdev->client.cci_i2c_master = dpbdev->cci_master_id;
	dpbdev->client.sid            = (u16)dpbdev->slave_addr >> 1;
	dpbdev->client.retries        = 3;
	dpbdev->client.id_map         = 0;
	dpbdev->client.i2c_freq_mode  = DPIN_CCI_UTIL_I2C_FAST_MODE;

	/* 3. MSM_CCI_INIT */
	rc = dpin_cci_util_cmd(&dpbdev->client, DPIN_CCI_UTIL_MSM_CCI_INIT);
	if (rc < 0) {
		dev_err(&dpbdev->ppdev->dev,
			"[dpin_cci_util] MSM_CCI_INIT failed rc=%d\n", rc);
		return rc;
	}
	dev_dbg(&dpbdev->ppdev->dev,
		 "client.sid:0x%x MSM_CCI_INIT OK (cci_dev=%d master=%d slave=0x%02x)\n",
		 dpbdev->client.sid, dpbdev->cci_dev_index, dpbdev->cci_master_id,
		 dpbdev->slave_addr);
	return 0;
}

static int cci_ensure_configured(struct cci_util_dev *dev)
{
	int rc;

	if (dev->configured)
		return 0;

	rc = cci_configure(dev);
	if (rc < 0)
		return rc;

	dev->configured = true;
	return 0;
}

/**
 * cci_util_lt7911_enable_i2c - open the LT7911 I2C gate via CCI.
 * @handle: opaque device handle obtained from cci_util_lt7911_get_device()
 *
 * Writes the i2c_enable register sequence to the LT7911, allowing subsequent
 * I2C register accesses.
 */
void cci_util_lt7911_enable_i2c(struct cci_util_handle *handle)
{
	int rc;
	struct dpin_cci_util_i2c_reg_setting setting = { };
	struct cci_util_dev *dev;

	if (!handle || !handle->dev)
		return;

	mutex_lock(&g_cci_util_lock);
	dev = handle->dev;

	rc = cci_ensure_configured(dev);
	if (rc < 0) {
		mutex_unlock(&g_cci_util_lock);
		return;
	}

	setting.reg_setting = i2c_enable;
	setting.size        = ARRAY_SIZE(i2c_enable);
	setting.addr_type   = DPIN_CCI_UTIL_I2C_TYPE_BYTE;
	setting.data_type   = DPIN_CCI_UTIL_I2C_TYPE_BYTE;
	setting.delay       = 0;

	/* Execute write */
	rc = dpin_cci_util_write(&dev->client, &setting);
	mutex_unlock(&g_cci_util_lock);

	if (rc < 0) {
		dev_err(&dev->ppdev->dev,
			"[dpin_cci_util] DBG1 CCI write failed rc=%d\n", rc);
		return;
	}
	dev_dbg(&dev->ppdev->dev,
		 "%s [dpin_cci_util] DBG1 CCI write OK - wrote %zu regs to slave 0x%02x\n",
		 __func__, ARRAY_SIZE(i2c_enable), dev->slave_addr);
	msleep(100);
}
EXPORT_SYMBOL_GPL(cci_util_lt7911_enable_i2c);

/**
 * cci_util_lt7911_disable_i2c - close the LT7911 I2C gate via CCI.
 * @handle: opaque device handle obtained from cci_util_lt7911_get_device()
 *
 * Writes the i2c_disable register sequence to the LT7911, blocking further
 * I2C register accesses until the gate is re-opened.
 */
void cci_util_lt7911_disable_i2c(struct cci_util_handle *handle)
{
	int rc;
	struct dpin_cci_util_i2c_reg_setting setting = { };
	struct cci_util_dev *dev;

	if (!handle || !handle->dev)
		return;

	mutex_lock(&g_cci_util_lock);
	dev = handle->dev;

	rc = cci_ensure_configured(dev);
	if (rc < 0) {
		mutex_unlock(&g_cci_util_lock);
		return;
	}

	setting.reg_setting = i2c_disable;
	setting.size        = ARRAY_SIZE(i2c_disable);
	setting.addr_type   = DPIN_CCI_UTIL_I2C_TYPE_BYTE;
	setting.data_type   = DPIN_CCI_UTIL_I2C_TYPE_BYTE;
	setting.delay       = 0;

	/* Execute write */
	rc = dpin_cci_util_write(&dev->client, &setting);
	mutex_unlock(&g_cci_util_lock);

	if (rc < 0) {
		dev_err(&dev->ppdev->dev,
			"[dpin_cci_util] DBG1 CCI write failed rc=%d\n", rc);
		return;
	}
	dev_dbg(&dev->ppdev->dev,
		 "%s [dpin_cci_util] DBG1 CCI write OK - wrote %zu regs to slave 0x%02x\n",
		 __func__, ARRAY_SIZE(i2c_disable), dev->slave_addr);
	msleep(100);
}
EXPORT_SYMBOL_GPL(cci_util_lt7911_disable_i2c);

static void cci_util_release(struct cci_util_dev *dpbdev)
{
	int rc;

	/* MSM_CCI_RELEASE (always, regardless of earlier result) */
	rc = dpin_cci_util_cmd(&dpbdev->client, DPIN_CCI_UTIL_MSM_CCI_RELEASE);
	if (rc < 0)
		dev_err(&dpbdev->ppdev->dev,
			"[dpin_cci_util] MSM_CCI_RELEASE failed rc=%d\n", rc);
	else
		dev_dbg(&dpbdev->ppdev->dev, "[dpin_cci_util] MSM_CCI_RELEASE OK\n");
}

/**
 * cci_util_lt7911_release_cci - release CCI power domain and clock votes.
 * @handle: opaque device handle obtained from cci_util_lt7911_get_device()
 *
 * Sends MSM_CCI_RELEASE to release the camera subsystem power domain (GDSC)
 * and clocks, allowing SoC / AOSD power collapse (sleep).
 */
void cci_util_lt7911_release_cci(struct cci_util_handle *handle)
{
	struct cci_util_dev *dev;

	if (!handle || !handle->dev)
		return;

	mutex_lock(&g_cci_util_lock);
	dev = handle->dev;
	if (dev && dev->configured) {
		cci_util_release(dev);
		dev->configured = false;
	}
	mutex_unlock(&g_cci_util_lock);
}
EXPORT_SYMBOL_GPL(cci_util_lt7911_release_cci);

/**
 * dpin_cci_util_read_seq - sequential byte read of @num_bytes registers
 *                          starting at @reg_addr into @out_buf.
 * @client:    populated CCI client
 * @reg_addr:  first register address to read
 * @out_buf:   caller-allocated buffer of at least @num_bytes bytes
 * @num_bytes: number of consecutive bytes to read
 *
 * Mirrors cam_camera_cci_i2c_read_seq() / camera_io_dev_read_seq() from
 * cam_sensor_cci_i2c.c, adapted to use the local dpin_cci_util_ctrl path.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int dpin_cci_util_read_seq(struct dpin_cci_util_sensor_client *client,
			     u32 reg_addr,
			     u8 *out_buf,
			     u16 num_bytes)
{
	int rc;
	struct dpin_cci_util_ctrl ctrl;
	u8 *buf;

	if (!out_buf || num_bytes == 0)
		return -EINVAL;

	buf = kzalloc(num_bytes, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.cmd                              = DPIN_CCI_UTIL_MSM_CCI_I2C_READ;
	ctrl.cci_info                         = client;
	ctrl.cfg.cci_i2c_read_cfg.addr        = reg_addr;
	ctrl.cfg.cci_i2c_read_cfg.addr_type   = (u16)DPIN_CCI_UTIL_I2C_TYPE_BYTE;
	ctrl.cfg.cci_i2c_read_cfg.data_type   = (u16)DPIN_CCI_UTIL_I2C_TYPE_BYTE;
	ctrl.cfg.cci_i2c_read_cfg.num_byte    = num_bytes;
	ctrl.cfg.cci_i2c_read_cfg.data        = buf;

	rc = v4l2_subdev_call(client->cci_subdev, core, ioctl,
			      DPIN_CCI_UTIL_VIDIOC_MSM_CCI_CFG, &ctrl);
	if (rc < 0) {
		dev_err(client->cci_subdev->dev,
			"%s v4l2_subdev_call failed rc=%d\n", __func__, rc);
		goto out;
	}
	rc = ctrl.status;
	if (rc < 0) {
		dev_err(client->cci_subdev->dev,
			"%s CCI I2C transaction failed status=%d\n",
			__func__, rc);
		goto out;
	}
	memcpy(out_buf, buf, num_bytes);

out:
	kfree(buf);
	return rc;
}

/**
 * lt7911_get_information - read and log the LT7911 video-timing and
 *                          port-configuration registers over I2C.
 * @dpbdev:    driver instance with an initialised CCI client
 * @read_seq0: caller-allocated buffer of LT7911_INFORMATION_FROM_84_NUM bytes
 *             for interrupt-type and video-timing data (regs 0x84-0x94)
 * @read_seq1: caller-allocated buffer of LT7911_INFORMATION_FROM_A0_NUM bytes
 *             for port-num, MIPI format and audio-channel data (regs 0xA0-0xA2)
 *
 * Reads two register banks:
 *   0x84-0x94  (LT7911_INFORMATION_FROM_84_NUM = 17 bytes)
 *   0xA0-0xA2  (LT7911_INFORMATION_FROM_A0_NUM =  3 bytes)
 *
 * The I2C gate must already be open (enable_i2c called) before calling.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int lt7911_get_information(struct cci_util_dev *dpbdev, u8 *read_seq0, u8 *read_seq1)
{
	int rc = 0, i, val = 0;
	struct dpin_cci_util_i2c_reg_setting setting = { };
	struct dpin_cci_util_i2c_reg_array bank_sel[] = {
		{ .reg_addr = 0xff, .reg_data = 0xe0,
		  .delay = 0x00, .data_mask = 0x00 },
	};

	setting.reg_setting = bank_sel;
	setting.size        = ARRAY_SIZE(bank_sel);
	setting.addr_type   = DPIN_CCI_UTIL_I2C_TYPE_BYTE;
	setting.data_type   = DPIN_CCI_UTIL_I2C_TYPE_BYTE;
	setting.delay       = 0;

	rc = dpin_cci_util_write(&dpbdev->client, &setting);
	if (rc < 0) {
		dev_err(&dpbdev->ppdev->dev,
			"%s bank-select write failed rc=%d\n", __func__, rc);
		return rc;
	}

	/* Read 0x84..0x94: interrupt type + video timing registers */
	rc = dpin_cci_util_read_seq(&dpbdev->client,
			       LT7911UXC_INTERRUPT_TYPE_REG,
			       read_seq0,
			       LT7911_INFORMATION_FROM_84_NUM);
	if (rc < 0) {
		dev_err(&dpbdev->ppdev->dev,
			"%s read_seq 0x84 failed rc=%d\n", __func__, rc);
		return rc;
	}

	for (i = 0; i < LT7911_INFORMATION_FROM_84_NUM; i++)
		val |= read_seq0[i];
	if (val) {
		for (i = 0; i < LT7911_INFORMATION_FROM_84_NUM; i++)
			dev_dbg(&dpbdev->ppdev->dev,
				 "[lt7911] reg 0x%02x = 0x%02x\n",
				 LT7911UXC_INTERRUPT_TYPE_REG + i, read_seq0[i]);
	} else {
		dev_dbg(&dpbdev->ppdev->dev,
			 "[lt7911] All Interrupt type registers are 0\n");
	}
	/* Read 0xA0..0xA2: port-num, MIPI format, audio channel */
	rc = dpin_cci_util_read_seq(&dpbdev->client,
			       LT7911UXC_PORT_NUM_REG,
			       read_seq1,
			       LT7911_INFORMATION_FROM_A0_NUM);
	if (rc < 0) {
		dev_err(&dpbdev->ppdev->dev,
			"%s read_seq 0xA0 failed rc=%d\n", __func__, rc);
		return rc;
	}

	for (val = 0, i = 0; i < LT7911_INFORMATION_FROM_A0_NUM; i++)
		val |= read_seq1[i];
	if (val) {
		for (i = 0; i < LT7911_INFORMATION_FROM_A0_NUM; i++)
			dev_dbg(&dpbdev->ppdev->dev,
				 "[lt7911] reg 0x%02x = 0x%02x\n",
				 LT7911UXC_PORT_NUM_REG + i, read_seq1[i]);
	} else {
		dev_dbg(&dpbdev->ppdev->dev,
			 "[lt7911] All Information registers are 0\n");
	}
	return 0;
}

static int read_chip_id(struct cci_util_dev *dpbdev)
{
	int rc;
	u32 read_buf = 0;
	u8 reg_addr = 0x00;

	rc = dpin_cci_util_read(&dpbdev->client,
			reg_addr,
			DPIN_CCI_UTIL_I2C_TYPE_BYTE,
			DPIN_CCI_UTIL_I2C_TYPE_WORD,
			&read_buf,
			2);
	if (rc < 0) {
		dev_err(&dpbdev->ppdev->dev,
			"[dpin_cci_util] CCI read failed at reg 0x%04x rc=%d\n",
			reg_addr, rc);
		return rc;
	}
	dev_dbg(&dpbdev->ppdev->dev,
		 "[dpin_cci_util] reg 0x%02x: read_buf 0x%x expected 0x2204\n",
		 reg_addr, read_buf);
	return 0;
}

/**
 * cci_util_lt7911_read_chip_id - read and log the LT7911 chip ID over CCI.
 * @handle: opaque device handle obtained from cci_util_lt7911_get_device()
 *
 * Configures the CCI client on first use, opens the I2C gate, reads the
 * two-byte chip-ID from register 0x00 (expected value 0x2204), then closes
 * the gate.  The entire sequence is performed under g_cci_util_lock.
 *
 * Return: 0 on success, negative errno on failure.
 */
int cci_util_lt7911_read_chip_id(struct cci_util_handle *handle)
{
	int rc;
	struct cci_util_dev *dev;
	struct dpin_cci_util_i2c_reg_setting setting = { };

	if (!handle || !handle->dev)
		return -EINVAL;

	mutex_lock(&g_cci_util_lock);
	dev = handle->dev;

	rc = cci_ensure_configured(dev);
	if (rc < 0) {
		mutex_unlock(&g_cci_util_lock);
		return rc;
	}

	/* enable I2C gate inline (no re-read of g_cci_util needed) */
	setting.reg_setting = i2c_enable;
	setting.size        = ARRAY_SIZE(i2c_enable);
	setting.addr_type   = DPIN_CCI_UTIL_I2C_TYPE_BYTE;
	setting.data_type   = DPIN_CCI_UTIL_I2C_TYPE_BYTE;
	setting.delay       = 0;
	rc = dpin_cci_util_write(&dev->client, &setting);
	if (rc < 0) {
		dev_err(&dev->ppdev->dev,
			"[dpin_cci_util] enable_i2c write failed rc=%d\n", rc);
		mutex_unlock(&g_cci_util_lock);
		return rc;
	}

	rc = read_chip_id(dev);

	/* disable I2C gate inline */
	setting.reg_setting = i2c_disable;
	setting.size        = ARRAY_SIZE(i2c_disable);
	dpin_cci_util_write(&dev->client, &setting);

	mutex_unlock(&g_cci_util_lock);
	return rc;
}
EXPORT_SYMBOL_GPL(cci_util_lt7911_read_chip_id);

/**
 * cci_util_lt7911_get_information - read LT7911 video-timing and port-config
 *                                   registers and decode them into caller fields.
 * @handle: opaque device handle obtained from cci_util_lt7911_get_device()
 * @irq:    receives the interrupt-type value (reg 0x84)
 * @width:  receives the active horizontal resolution in pixels
 * @height: receives the active vertical resolution in lines
 * @fps:    receives the frame rate x 100 (e.g. 6000 = 60.00 Hz)
 * @format: receives the MIPI output format code (reg 0xA1)
 * @afreq:  receives the audio sample frequency in kHz
 * @ach:    receives the audio channel count (reg 0xA2)
 *
 * These registers (0x84-0x94, 0xA0-0xA2) are VIDEO SIGNAL STATUS registers
 * populated by the LT7911 only after it has locked onto an active HDMI/DP
 * input signal.  The I2C gate must already be open before calling.
 *
 * Return: 0 on success, negative errno on failure.
 */
int cci_util_lt7911_get_information(struct cci_util_handle *handle,
					int *irq, int *width, int *height,
					int *fps, int *format, int *afreq, int *ach)
{
	int rc;
	u8 read_seq0[LT7911_INFORMATION_FROM_84_NUM] = { 0 };
	u8 read_seq1[LT7911_INFORMATION_FROM_A0_NUM] = { 0 };
	u32 p_clock, h_total, v_total, hv_total;
	u64 fps_calc;
	struct cci_util_dev *dev;

	if (!handle || !handle->dev)
		return -EINVAL;

	mutex_lock(&g_cci_util_lock);
	dev = handle->dev;

	rc = cci_ensure_configured(dev);
	if (rc < 0) {
		mutex_unlock(&g_cci_util_lock);
		return rc;
	}

	rc = lt7911_get_information(dev, read_seq0, read_seq1);
	if (!rc) {
		*irq = read_seq0[LT7911UXC_INTERRUPT_TYPE_IDX];
		*width = (read_seq0[LT7911UXC_HACTIVE_MSB_IDX] << 8) |
						read_seq0[LT7911UXC_HACTIVE_LSB_IDX];
		*height = ((read_seq0[LT7911UXC_VACTIVE_MSB_IDX] << 8) |
						read_seq0[LT7911UXC_VACTIVE_LSB_IDX]);
		*format = read_seq1[LT7911UXC_MIPI_FORMAT_IDX];
		p_clock = (read_seq0[LT7911UXC_PCLOCK_3_IDX] << 16) |
						(read_seq0[LT7911UXC_PCLOCK_2_IDX] << 8) |
						read_seq0[LT7911UXC_PCLOCK_1_IDX];
		h_total = (read_seq0[LT7911UXC_HTOTAL_MSB_IDX] << 8) |
						read_seq0[LT7911UXC_HTOTAL_LSB_IDX];
		v_total = (read_seq0[LT7911UXC_VTOTAL_MSB_IDX] << 8) |
						read_seq0[LT7911UXC_VTOTAL_LSB_IDX];
		fps_calc = (u64)p_clock * 100000ULL;
		hv_total = h_total * v_total;
		*fps = (hv_total > 0) ? (int)div_u64(fps_calc, hv_total) : 0;
		*afreq = ((read_seq0[LT7911UXC_AUDIO_FREQ_MSB_IDX] << 8) |
						read_seq0[LT7911UXC_AUDIO_FREQ_LSB_IDX]);
		*ach = read_seq1[LT7911UXC_AUDIO_CHANNEL_IDX];
	}

	mutex_unlock(&g_cci_util_lock);
	return rc;
}
EXPORT_SYMBOL_GPL(cci_util_lt7911_get_information);

/**
 * lt7911_write_regs - write a static register array to the LT7911 via CCI.
 * @client:   populated CCI client
 * @regs:     array of register address/data/delay/mask entries to write
 * @num_regs: number of entries in @regs
 *
 * Return: 0 on success, negative errno on failure.
 */
static int lt7911_write_regs(struct dpin_cci_util_sensor_client *client,
			     struct dpin_cci_util_i2c_reg_array *regs,
			     u32 num_regs)
{
	struct dpin_cci_util_i2c_reg_setting s = {
		.reg_setting = regs,
		.size        = num_regs,
		.addr_type   = DPIN_CCI_UTIL_I2C_TYPE_BYTE,
		.data_type   = DPIN_CCI_UTIL_I2C_TYPE_BYTE,
		.delay       = 0,
	};
	return dpin_cci_util_write(client, &s);
}

/**
 * cci_util_lt7911_get_interrupt_type - read the LT7911UXC interrupt-type
 *                                      register (0x84).
 * @handle:   opaque device handle obtained from cci_util_lt7911_get_device()
 * @irq_type: output pointer that receives the raw register value on success
 *
 * Selects register bank 0xe0 then reads a single byte from register 0x84.
 * The I2C gate must already be open before calling.
 *
 * Return: 0 on success, negative errno on failure.
 */
int cci_util_lt7911_get_interrupt_type(struct cci_util_handle *handle, int *irq_type)
{
	int rc;
	u32 val = 0;
	struct cci_util_dev *dev;
	struct dpin_cci_util_i2c_reg_array bank_sel[] = {
		{ .reg_addr = 0xff, .reg_data = 0xe0,
		  .delay = 0x00, .data_mask = 0x00 },
	};

	if (!handle || !handle->dev || !irq_type)
		return -EINVAL;

	mutex_lock(&g_cci_util_lock);
	dev = handle->dev;

	rc = cci_ensure_configured(dev);
	if (rc < 0) {
		mutex_unlock(&g_cci_util_lock);
		return rc;
	}

	rc = lt7911_write_regs(&dev->client, bank_sel, ARRAY_SIZE(bank_sel));
	if (rc < 0) {
		dev_err(&dev->ppdev->dev,
			"%s bank-select write failed rc=%d\n", __func__, rc);
		mutex_unlock(&g_cci_util_lock);
		return rc;
	}

	rc = dpin_cci_util_read(&dev->client,
				LT7911UXC_INTERRUPT_TYPE_REG,
				DPIN_CCI_UTIL_I2C_TYPE_BYTE,
				DPIN_CCI_UTIL_I2C_TYPE_BYTE,
				&val, 1);
	mutex_unlock(&g_cci_util_lock);

	if (rc < 0) {
		dev_err(&dev->ppdev->dev,
			"%s interrupt type reg read failed rc=%d\n", __func__, rc);
		return rc;
	}

	*irq_type = (int)val;
	dev_dbg(&dev->ppdev->dev,
		 "%s LT7911UXC_INTERRUPT_TYPE_REG(0x%02x) = 0x%02x\n",
		 __func__, LT7911UXC_INTERRUPT_TYPE_REG, *irq_type);
	return 0;
}
EXPORT_SYMBOL_GPL(cci_util_lt7911_get_interrupt_type);

static unsigned int lt7911_get_crc_val(struct lt7911_crc_info type,
				       uint8_t *buf, uint32_t buf_len)
{
	uint8_t  width   = type.width;
	uint32_t poly    = type.poly;
	uint32_t crc     = type.crc_init;
	uint32_t xor_out = type.xor_out;
	bool     ref_in  = type.ref_in;
	bool     ref_out = type.ref_out;
	uint8_t  n;
	uint32_t bits;
	uint32_t data;
	uint8_t  i;

	n    = (width < 8) ? 0 : (width - 8);
	crc  = (width < 8) ? (crc << (8 - width)) : crc;
	bits = (width < 8) ? 0x80U : (1U << (width - 1));
	poly = (width < 8) ? (poly << (8 - width)) : poly;

	while (buf_len--) {
		data = *(buf++);
		if (ref_in)
			data = bitrev8(data);
		crc ^= (data << n);
		for (i = 0; i < 8; i++) {
			if (crc & bits)
				crc = (crc << 1) ^ poly;
			else
				crc = crc << 1;
		}
	}
	crc = (width < 8) ? (crc >> (8 - width)) : crc;
	if (ref_out)
		crc = bitrev8(crc);
	crc ^= xor_out;
	return (crc & ((2U << (width - 1)) - 1));
}

static uint8_t lt7911uxc_get_crc(uint8_t *upgrade_data, uint32_t len)
{
	struct lt7911_crc_info type = {
		.width    = 8,
		.poly     = 0x31,
		.crc_init = 0,
		.xor_out  = 0,
		.ref_out  = false,
		.ref_in   = false,
	};
	uint32_t crc_size = LT7911UXC_FW_AREA_SIZE - 1;
	uint8_t  default_val = 0xFF;

	type.crc_init = lt7911_get_crc_val(type, upgrade_data, len);
	crc_size -= len;
	while (crc_size--)
		type.crc_init = lt7911_get_crc_val(type, &default_val, 1);
	return type.crc_init;
}

/*
 * LT7911 firmware upgrade — low-level flash helpers
 */

/**
 * lt7911_write_burst - write a byte array to a single register via CCI burst.
 * @client: populated CCI client
 * @reg:    register address to write each byte to
 * @buf:    source data buffer
 * @size:   number of bytes to write (1 to LT7911_BYTESIZE_PER_PAGE)
 *
 * Return: 0 on success, negative errno on failure.
 */
static int lt7911_write_burst(struct dpin_cci_util_sensor_client *client,
			      u8 reg, const uint8_t *buf, int size)
{
	int rc, i;
	struct dpin_cci_util_i2c_reg_array *arr;
	struct dpin_cci_util_i2c_reg_setting s;

	if (size <= 0 || size > LT7911_BYTESIZE_PER_PAGE)
		return -EINVAL;

	arr = kcalloc(size, sizeof(*arr), GFP_KERNEL);
	if (!arr)
		return -ENOMEM;

	for (i = 0; i < size; i++) {
		arr[i].reg_addr  = reg;
		arr[i].reg_data  = buf[i];
		arr[i].delay     = 0;
		arr[i].data_mask = 0;
	}

	s.reg_setting = arr;
	s.size        = size;
	s.addr_type   = DPIN_CCI_UTIL_I2C_TYPE_BYTE;
	s.data_type   = DPIN_CCI_UTIL_I2C_TYPE_BYTE;
	s.delay       = 0;

	rc = dpin_cci_util_write(client, &s);
	kfree(arr);
	return rc;
}

static int lt7911uxc_config(struct dpin_cci_util_sensor_client *client)
{
	struct dpin_cci_util_i2c_reg_array reg_cfg[] = {
		{ .reg_addr = 0xff, .reg_data = 0xe0, .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0xee, .reg_data = 0x01, .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5e, .reg_data = 0xc1, .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x58, .reg_data = 0x00, .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x59, .reg_data = 0x50, .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5a, .reg_data = 0x10, .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5a, .reg_data = 0x00, .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x58, .reg_data = 0x21, .delay = 0x00, .data_mask = 0x00 },
	};
	return lt7911_write_regs(client, reg_cfg, ARRAY_SIZE(reg_cfg));
}

static int lt7911uxc_flash_write_en(struct dpin_cci_util_sensor_client *client)
{
	struct dpin_cci_util_i2c_reg_array reg_cfg[] = {
		{ .reg_addr = 0xff, .reg_data = 0xe1, .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x03, .reg_data = 0x2e, .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x03, .reg_data = 0xee, .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0xff, .reg_data = 0xe0, .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5a, .reg_data = 0x04, .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5a, .reg_data = 0x00, .delay = 0x00, .data_mask = 0x00 },
	};
	return lt7911_write_regs(client, reg_cfg, ARRAY_SIZE(reg_cfg));
}

static int lt7911uxc_flash_write_forbid_config(
		struct dpin_cci_util_sensor_client *client)
{
	struct dpin_cci_util_i2c_reg_array reg_cfg[] = {
		{ .reg_addr = 0x5a, .reg_data = 0x08, .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5a, .reg_data = 0x00, .delay = 0x00, .data_mask = 0x00 },
	};
	return lt7911_write_regs(client, reg_cfg, ARRAY_SIZE(reg_cfg));
}

static int lt7911uxc_flash_write_page_config(
		struct dpin_cci_util_sensor_client *client)
{
	struct dpin_cci_util_i2c_reg_array reg_cfg[] = {
		{ .reg_addr = 0x5e, .reg_data = 0xdf, .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5a, .reg_data = 0x20, .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5a, .reg_data = 0x00, .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x58, .reg_data = 0x21, .delay = 0x00, .data_mask = 0x00 },
	};
	return lt7911_write_regs(client, reg_cfg, ARRAY_SIZE(reg_cfg));
}

static int lt7911uxc_flash_write_addr_set(
		struct dpin_cci_util_sensor_client *client, uint8_t *addr)
{
	struct dpin_cci_util_i2c_reg_array reg_cfg[] = {
		{ .reg_addr = 0x5b, .reg_data = addr[0], .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5c, .reg_data = addr[1], .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5d, .reg_data = addr[2], .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5a, .reg_data = 0x10,    .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5a, .reg_data = 0x00,    .delay = 0x00, .data_mask = 0x00 },
	};
	return lt7911_write_regs(client, reg_cfg, ARRAY_SIZE(reg_cfg));
}

static int lt7911uxc_flash_read_addr_set(
		struct dpin_cci_util_sensor_client *client, uint8_t *addr)
{
	struct dpin_cci_util_i2c_reg_array reg_cfg[] = {
		{ .reg_addr = 0x5e, .reg_data = 0x5f,    .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5a, .reg_data = 0x20,    .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5a, .reg_data = 0x00,    .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5b, .reg_data = addr[0], .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5c, .reg_data = addr[1], .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5d, .reg_data = addr[2], .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5a, .reg_data = 0x10,    .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5a, .reg_data = 0x00,    .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x58, .reg_data = 0x21,    .delay = 0x00, .data_mask = 0x00 },
	};
	return lt7911_write_regs(client, reg_cfg, ARRAY_SIZE(reg_cfg));
}

static int lt7911uxc_block_erase(struct dpin_cci_util_sensor_client *client)
{
	int rc;
	struct dpin_cci_util_i2c_reg_array reg_cfg1[] = {
		{ .reg_addr = 0x5a, .reg_data = 0x04, .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5a, .reg_data = 0x00, .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5b, .reg_data = 0x00, .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5c, .reg_data = 0x00, .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5d, .reg_data = 0x00, .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5a, .reg_data = 0x01, .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5a, .reg_data = 0x00, .delay = 0x00, .data_mask = 0x00 },
	};
	struct dpin_cci_util_i2c_reg_array reg_cfg2[] = {
		{ .reg_addr = 0x5a, .reg_data = 0x04, .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5a, .reg_data = 0x00, .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5b, .reg_data = 0x00, .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5c, .reg_data = 0x80, .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5d, .reg_data = 0x00, .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5a, .reg_data = 0x01, .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5a, .reg_data = 0x00, .delay = 0x00, .data_mask = 0x00 },
	};

	/* Erase first 32 KB half-block */
	rc = lt7911_write_regs(client, reg_cfg1, ARRAY_SIZE(reg_cfg1));
	if (rc < 0) {
		dev_err(client->cci_subdev->dev,
			"[lt7911_fw] block erase (first half) failed rc=%d\n", rc);
		return rc;
	}
	msleep(700);

	/* Erase second 32 KB half-block */
	rc = lt7911_write_regs(client, reg_cfg2, ARRAY_SIZE(reg_cfg2));
	if (rc < 0) {
		dev_err(client->cci_subdev->dev,
			"[lt7911_fw] block erase (second half) failed rc=%d\n", rc);
		return rc;
	}
	msleep(700);
	return rc;
}

static int lt7911uxc_firmware_write(
		struct dpin_cci_util_sensor_client *client,
		const uint8_t *fw_data, int size)
{
	uint8_t addr[3] = { 0, 0, 0 };
	int rc = 0;
	int n_pages = (size % LT7911_BYTESIZE_PER_PAGE)
		? ((size / LT7911_BYTESIZE_PER_PAGE) + 1)
		: (size / LT7911_BYTESIZE_PER_PAGE);
	bool last_partial = (size % LT7911_BYTESIZE_PER_PAGE) != 0;
	int  last_size    = size % LT7911_BYTESIZE_PER_PAGE;
	unsigned int start_addr = 0;
	int j, cur_len;
	uint8_t *write_buf;

	write_buf = kzalloc(LT7911_BYTESIZE_PER_PAGE, GFP_KERNEL);
	if (!write_buf)
		return -ENOMEM;

	dev_dbg(client->cci_subdev->dev,
		 "[lt7911_fw] writing %d pages (last_partial=%d last_size=%d)\n",
		 n_pages, last_partial, last_size);

	for (j = 0; j < n_pages; ++j) {
		rc = lt7911uxc_flash_write_en(client);
		if (rc < 0) {
			dev_err(client->cci_subdev->dev,
				"[lt7911_fw] flash_write_en failed rc=%d\n", rc);
			goto out;
		}
		rc = lt7911uxc_flash_write_page_config(client);
		if (rc < 0) {
			dev_err(client->cci_subdev->dev,
				"[lt7911_fw] flash_write_page_config failed rc=%d\n", rc);
			goto out;
		}

		cur_len = (j == n_pages - 1 && last_partial)
			? last_size : LT7911_BYTESIZE_PER_PAGE;

		memset(write_buf, 0xff, LT7911_BYTESIZE_PER_PAGE);
		memcpy(write_buf, fw_data + start_addr, cur_len);

		rc = lt7911_write_burst(client, 0x59, write_buf, cur_len);
		if (rc < 0) {
			dev_err(client->cci_subdev->dev,
				"[lt7911_fw] burst write failed rc=%d\n", rc);
			goto out;
		}

		rc = lt7911uxc_flash_write_addr_set(client, addr);
		if (rc < 0) {
			dev_err(client->cci_subdev->dev,
				"[lt7911_fw] flash_write_addr_set failed rc=%d\n", rc);
			goto out;
		}

		start_addr += LT7911_BYTESIZE_PER_PAGE;
		addr[0] = (start_addr & 0xFF0000) >> 16;
		addr[1] = (start_addr & 0x00FF00) >> 8;
		addr[2] =  start_addr & 0x0000FF;
	}

	dev_dbg(client->cci_subdev->dev,
		 "[lt7911_fw] write done start_addr=0x%x addr=[0x%x,0x%x,0x%x]\n",
		 start_addr, addr[0], addr[1], addr[2]);

	rc = lt7911uxc_flash_write_forbid_config(client);
	if (rc < 0)
		dev_err(client->cci_subdev->dev,
			"[lt7911_fw] flash_write_forbid_config failed rc=%d\n", rc);
out:
	kfree(write_buf);
	return rc;
}

static int lt7911uxc_firmware_write_crc(
		struct dpin_cci_util_sensor_client *client,
		const uint8_t *fw_data, int size)
{
	uint8_t addr[3] = { 0x00, 0xff, 0xff };
	uint8_t crc_byte;
	int rc;
	struct dpin_cci_util_i2c_reg_array reg_cfg[] = {
		{ .reg_addr = 0x5e, .reg_data = 0xc0, .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5a, .reg_data = 0x20, .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x5a, .reg_data = 0x00, .delay = 0x00, .data_mask = 0x00 },
		{ .reg_addr = 0x58, .reg_data = 0x21, .delay = 0x00, .data_mask = 0x00 },
	};

	crc_byte = lt7911uxc_get_crc((uint8_t *)fw_data, size);
	dev_dbg(client->cci_subdev->dev,
		 "[lt7911_fw] CRC byte = 0x%02x\n", crc_byte);

	rc = lt7911uxc_flash_write_en(client);
	if (rc < 0) {
		dev_err(client->cci_subdev->dev,
			"[lt7911_fw] flash_write_en (crc) failed rc=%d\n", rc);
		return rc;
	}

	rc = lt7911_write_regs(client, reg_cfg, ARRAY_SIZE(reg_cfg));
	if (rc < 0) {
		dev_err(client->cci_subdev->dev,
			"[lt7911_fw] crc page-config write failed rc=%d\n", rc);
		return rc;
	}

	rc = lt7911_write_burst(client, 0x59, &crc_byte, 1);
	if (rc < 0) {
		dev_err(client->cci_subdev->dev,
			"[lt7911_fw] crc burst write failed rc=%d\n", rc);
		return rc;
	}

	rc = lt7911uxc_flash_write_addr_set(client, addr);
	if (rc < 0) {
		dev_err(client->cci_subdev->dev,
			"[lt7911_fw] flash_write_addr_set (crc) failed rc=%d\n", rc);
		return rc;
	}

	rc = lt7911uxc_flash_write_forbid_config(client);
	if (rc < 0)
		dev_err(client->cci_subdev->dev,
			"[lt7911_fw] flash_write_forbid_config (crc) failed rc=%d\n", rc);
	return rc;
}

static int lt7911uxc_firmware_read_back(
		struct dpin_cci_util_sensor_client *client,
		uint8_t *buff, int size)
{
	unsigned int read_addr = 0;
	uint8_t addr[3] = { 0, 0, 0 };
	int i, j, cur_read_len;
	int n_page = size / LT7911_BYTESIZE_PER_PAGE;
	int rc = 0;
	uint8_t *cur_read_buf;

	if (size % LT7911_BYTESIZE_PER_PAGE != 0)
		++n_page;

	cur_read_buf = kzalloc(LT7911_BYTESIZE_PER_PAGE, GFP_KERNEL);
	if (!cur_read_buf)
		return -ENOMEM;

	for (i = 0; i < n_page; ++i) {
		rc = lt7911uxc_flash_read_addr_set(client, addr);
		if (rc < 0) {
			dev_err(client->cci_subdev->dev,
				"[lt7911_fw] flash_read_addr_set failed i=%d rc=%d\n",
				i, rc);
			goto out;
		}

		cur_read_len = ((size - i * LT7911_BYTESIZE_PER_PAGE) <
			       LT7911_BYTESIZE_PER_PAGE)
			? (size - i * LT7911_BYTESIZE_PER_PAGE)
			: LT7911_BYTESIZE_PER_PAGE;

		rc = dpin_cci_util_read_seq(client, LT7911_READNUM_SET_COMMAND,
					    cur_read_buf, cur_read_len);
		if (rc < 0) {
			dev_err(client->cci_subdev->dev,
				"[lt7911_fw] flash read failed i=%d rc=%d\n", i, rc);
			goto out;
		}

		for (j = 0; j < cur_read_len; ++j)
			buff[i * LT7911_BYTESIZE_PER_PAGE + j] = cur_read_buf[j];

		read_addr += cur_read_len;
		addr[0] = (read_addr & 0xFF0000) >> 16;
		addr[1] = (read_addr & 0x00FF00) >> 8;
		addr[2] =  read_addr & 0x0000FF;
	}
out:
	kfree(cur_read_buf);
	return rc;
}

/*
 * LT7911 firmware upgrade — version / HDCP key reads
 */

/**
 * cci_util_lt7911_get_version - read the 32-bit firmware version from the LT7911.
 * @handle: opaque device handle obtained from cci_util_lt7911_get_device()
 *
 * Selects register bank 0xe0, reads two 16-bit words from VERSION_REG0 (0x80)
 * and VERSION_REG1 (0x82), and assembles them into a single 32-bit version
 * word: (ver_word1 << 16) | ver_word2.
 *
 * The I2C gate must already be open and the CCI client configured before
 * calling this function.
 *
 * Return: 32-bit firmware version on success, 0 on error.
 */
uint32_t cci_util_lt7911_get_version(struct cci_util_handle *handle)
{
	int rc;
	u32 ver_word1 = 0, ver_word2 = 0;
	struct cci_util_dev *dev;
	struct dpin_cci_util_i2c_reg_array bank_sel[] = {
		{ .reg_addr = 0xff, .reg_data = 0xe0,
		  .delay = 0x00, .data_mask = 0x00 },
	};

	if (!handle || !handle->dev)
		return 0;

	mutex_lock(&g_cci_util_lock);
	dev = handle->dev;

	rc = cci_ensure_configured(dev);
	if (rc < 0) {
		mutex_unlock(&g_cci_util_lock);
		return 0;
	}

	rc = lt7911_write_regs(&dev->client, bank_sel, ARRAY_SIZE(bank_sel));
	if (rc < 0) {
		dev_err(&dev->ppdev->dev,
			"[lt7911_fw] bank-select write failed rc=%d\n", rc);
		mutex_unlock(&g_cci_util_lock);
		return 0;
	}

	rc = dpin_cci_util_read(&dev->client,
				LT7911UXC_VERSION_REG0,
				DPIN_CCI_UTIL_I2C_TYPE_BYTE,
				DPIN_CCI_UTIL_I2C_TYPE_WORD,
				&ver_word1, 2);
	if (rc < 0) {
		dev_err(&dev->ppdev->dev,
			"[lt7911_fw] version reg0 read failed rc=%d\n", rc);
		mutex_unlock(&g_cci_util_lock);
		return 0;
	}

	rc = dpin_cci_util_read(&dev->client,
				LT7911UXC_VERSION_REG1,
				DPIN_CCI_UTIL_I2C_TYPE_BYTE,
				DPIN_CCI_UTIL_I2C_TYPE_WORD,
				&ver_word2, 2);
	mutex_unlock(&g_cci_util_lock);

	if (rc < 0) {
		dev_err(&dev->ppdev->dev,
			"[lt7911_fw] version reg1 read failed rc=%d\n", rc);
		return 0;
	}

	dev_dbg(&dev->ppdev->dev,
		 "[lt7911_fw] version reg0=0x%x reg1=0x%x version=0x%x\n",
		 ver_word1, ver_word2, (ver_word1 << 16) | ver_word2);
	return (ver_word1 << 16) | ver_word2;
}
EXPORT_SYMBOL_GPL(cci_util_lt7911_get_version);

/**
 * cci_util_lt7911_read_hdcpkey - read HDCP key bytes from LT7911 flash.
 * @handle: opaque device handle obtained from cci_util_lt7911_get_device()
 * @buff: caller-allocated buffer of at least @size bytes
 * @size: number of bytes to read (typically LT7911_HDCPKEY_SIZE = 32)
 *
 * Calls lt7911uxc_config() to open the flash interface, then reads @size
 * bytes in 32-byte pages starting at LT7911_HDCPKEY_ADDR (0x050000).
 *
 * Return: 0 on success, negative errno on failure.
 */
int cci_util_lt7911_read_hdcpkey(struct cci_util_handle *handle, uint8_t *buff, int size)
{
	unsigned int read_addr = LT7911_HDCPKEY_ADDR;
	uint8_t addr[3];
	int i, j, cur_read_len;
	int n_page = size / LT7911_BYTESIZE_PER_PAGE;
	int rc = 0;
	uint8_t *cur_read_buf;
	struct cci_util_dev *dev;

	if (!handle || !handle->dev || !buff || size <= 0)
		return -EINVAL;

	mutex_lock(&g_cci_util_lock);
	dev = handle->dev;

	rc = cci_ensure_configured(dev);
	if (rc < 0) {
		mutex_unlock(&g_cci_util_lock);
		return rc;
	}

	if (size % LT7911_BYTESIZE_PER_PAGE != 0)
		++n_page;

	cur_read_buf = kzalloc(LT7911_BYTESIZE_PER_PAGE, GFP_KERNEL);
	if (!cur_read_buf) {
		mutex_unlock(&g_cci_util_lock);
		return -ENOMEM;
	}

	rc = lt7911uxc_config(&dev->client);
	if (rc < 0) {
		dev_err(&dev->ppdev->dev,
			"[lt7911_fw] lt7911uxc_config failed before HDCP key read rc=%d\n",
			rc);
		goto out;
	}

	addr[0] = (read_addr & 0xFF0000) >> 16;
	addr[1] = (read_addr & 0x00FF00) >> 8;
	addr[2] =  read_addr & 0x0000FF;

	for (i = 0; i < n_page; ++i) {
		rc = lt7911uxc_flash_read_addr_set(&dev->client, addr);
		if (rc < 0) {
			dev_err(&dev->ppdev->dev,
				"[lt7911_fw] flash_read_addr_set failed i=%d rc=%d\n",
				i, rc);
			goto out;
		}

		cur_read_len = ((size - i * LT7911_BYTESIZE_PER_PAGE) <
			       LT7911_BYTESIZE_PER_PAGE)
			? (size - i * LT7911_BYTESIZE_PER_PAGE)
			: LT7911_BYTESIZE_PER_PAGE;

		rc = dpin_cci_util_read_seq(&dev->client,
					    LT7911_READNUM_SET_COMMAND,
					    cur_read_buf, cur_read_len);
		if (rc < 0) {
			dev_err(&dev->ppdev->dev,
				"[lt7911_fw] HDCP key flash read failed i=%d rc=%d\n",
				i, rc);
			goto out;
		}

		for (j = 0; j < cur_read_len; ++j)
			buff[i * LT7911_BYTESIZE_PER_PAGE + j] = cur_read_buf[j];

		read_addr += cur_read_len;
		addr[0] = (read_addr & 0xFF0000) >> 16;
		addr[1] = (read_addr & 0x00FF00) >> 8;
		addr[2] =  read_addr & 0x0000FF;
	}
out:
	kfree(cur_read_buf);
	mutex_unlock(&g_cci_util_lock);
	return rc;
}
EXPORT_SYMBOL_GPL(cci_util_lt7911_read_hdcpkey);

/*
 * LT7911 firmware upgrade — top-level upgrade function
 */

/**
 * cci_util_lt7911_do_firmware_upgrade - perform a full LT7911 firmware upgrade.
 * @handle: opaque device handle obtained from cci_util_lt7911_get_device()
 * @fw: kernel firmware blob obtained via request_firmware()
 *
 * Executes the complete upgrade sequence under g_cci_util_lock:
 *   1. lt7911uxc_config()             - open flash interface
 *   2. lt7911uxc_block_erase()        - erase 64 KB (up to 3 retries)
 *   3. lt7911uxc_firmware_write()     - program firmware pages
 *   4. lt7911uxc_config()             - re-open for CRC write
 *   5. lt7911uxc_firmware_write_crc() - write CRC byte
 *   6. lt7911uxc_config()             - re-open for read-back
 *   7. lt7911uxc_firmware_read_back() - verify written data
 *
 * Return: 0 on success, negative errno on failure.
 */
int cci_util_lt7911_do_firmware_upgrade(struct cci_util_handle *handle,
					const struct firmware *fw)
{
	int i, rc = 0;
	uint8_t *fw_read_data = NULL;
	int fw_data_len;
	struct cci_util_dev *dev;

	if (!handle || !handle->dev)
		return -EINVAL;

	if (!fw || !fw->data || fw->size == 0 || fw->size >= LT7911UXC_FW_AREA_SIZE) {
		dev_err(&handle->dev->ppdev->dev,
			"[lt7911_fw] invalid firmware blob (size=%zu)\n",
			fw ? fw->size : 0);
		return -EINVAL;
	}

	fw_data_len = (int)fw->size;
	dev_dbg(&handle->dev->ppdev->dev,
		"[lt7911_fw] firmware size = %d bytes\n", fw_data_len);

	fw_read_data = kzalloc(fw_data_len, GFP_KERNEL);
	if (!fw_read_data)
		return -ENOMEM;

	mutex_lock(&g_cci_util_lock);
	dev = handle->dev;

	rc = cci_ensure_configured(dev);
	if (rc < 0) {
		mutex_unlock(&g_cci_util_lock);
		kfree(fw_read_data);
		return rc;
	}

	/* Step 1: open flash interface */
	rc = lt7911uxc_config(&dev->client);
	if (rc < 0) {
		dev_err(&dev->ppdev->dev,
			"[lt7911_fw] lt7911uxc_config failed rc=%d\n", rc);
		goto close;
	}

	/* Step 2: erase (up to 3 attempts) */
	for (i = 0; i < 3; i++) {
		rc = lt7911uxc_block_erase(&dev->client);
		if (rc == 0) {
			dev_dbg(&dev->ppdev->dev,
				 "[lt7911_fw] block erase succeeded (attempt %d)\n",
				 i + 1);
			break;
		}
		dev_err(&dev->ppdev->dev,
			"[lt7911_fw] block erase attempt %d failed rc=%d\n",
			i + 1, rc);
	}
	if (rc < 0) {
		dev_err(&dev->ppdev->dev,
			"[lt7911_fw] all erase attempts failed\n");
		goto close;
	}

	/* Step 3: write firmware */
	rc = lt7911uxc_firmware_write(&dev->client, fw->data, fw_data_len);
	if (rc < 0) {
		dev_err(&dev->ppdev->dev,
			"[lt7911_fw] firmware write failed rc=%d\n", rc);
		goto close;
	}
	msleep(20);

	/* Step 4: re-open for CRC write */
	rc = lt7911uxc_config(&dev->client);
	if (rc < 0) {
		dev_err(&dev->ppdev->dev,
			"[lt7911_fw] lt7911uxc_config (pre-CRC) failed rc=%d\n", rc);
		goto close;
	}

	/* Step 5: write CRC */
	rc = lt7911uxc_firmware_write_crc(&dev->client, fw->data, fw_data_len);
	if (rc < 0) {
		dev_err(&dev->ppdev->dev,
			"[lt7911_fw] firmware_write_crc failed rc=%d\n", rc);
		goto close;
	}

	/* Step 6: re-open for read-back */
	rc = lt7911uxc_config(&dev->client);
	if (rc < 0) {
		dev_err(&dev->ppdev->dev,
			"[lt7911_fw] lt7911uxc_config (pre-readback) failed rc=%d\n",
			rc);
		goto close;
	}

	/* Step 7: read back and verify */
	rc = lt7911uxc_firmware_read_back(&dev->client, fw_read_data, fw_data_len);
	if (rc < 0) {
		dev_err(&dev->ppdev->dev,
			"[lt7911_fw] firmware read-back failed rc=%d\n", rc);
		goto close;
	}

	if (!memcmp(fw->data, fw_read_data, fw_data_len)) {
		dev_dbg(&dev->ppdev->dev,
			 "[lt7911_fw] firmware upgrade SUCCESS\n");
		rc = 0;
	} else {
		dev_err(&dev->ppdev->dev,
			"[lt7911_fw] firmware upgrade FAILED (readback mismatch)\n");
		rc = -EIO;
	}

close:
	mutex_unlock(&g_cci_util_lock);
	kfree(fw_read_data);
	return rc;
}
EXPORT_SYMBOL_GPL(cci_util_lt7911_do_firmware_upgrade);

/**
 * cci_util_lt7911_reg_read - read a single byte register from the LT7911.
 * @handle:    opaque device handle obtained from cci_util_lt7911_get_device()
 * @reg:       register address to read (8-bit)
 * @reg_value: output pointer that receives the register value on success
 *
 * Acquires g_cci_util_lock, performs a single-byte CCI read, then releases
 * the lock.  The I2C gate must already be open before calling.
 *
 * Return: 0 on success, negative errno on failure.
 */
int cci_util_lt7911_reg_read(struct cci_util_handle *handle, int reg, int *reg_value)
{
	int rc;
	u32 val = 0;
	struct cci_util_dev *dev;

	if (!handle || !handle->dev)
		return -EINVAL;

	mutex_lock(&g_cci_util_lock);
	dev = handle->dev;

	rc = cci_ensure_configured(dev);
	if (rc < 0) {
		mutex_unlock(&g_cci_util_lock);
		return rc;
	}

	rc = dpin_cci_util_read(&dev->client,
				(u8)reg,
				DPIN_CCI_UTIL_I2C_TYPE_BYTE,
				DPIN_CCI_UTIL_I2C_TYPE_BYTE,
				&val, 1);
	mutex_unlock(&g_cci_util_lock);

	if (rc < 0) {
		dev_err(&dev->ppdev->dev,
			"[lt7911_fw] reg read failed reg=0x%02x rc=%d\n", reg, rc);
		return rc;
	}
	*reg_value = (int)val;

	return rc;
}
EXPORT_SYMBOL_GPL(cci_util_lt7911_reg_read);

/**
 * cci_util_lt7911_reg_write - write a register array to the LT7911 via CCI.
 * @handle:  opaque device handle obtained from cci_util_lt7911_get_device()
 * @reg_cfg: array of register address/data/delay/mask entries to write
 * @reg_sz:  number of entries in @reg_cfg
 *
 * Acquires g_cci_util_lock, delegates to lt7911_write_regs(), then releases
 * the lock.  The I2C gate must already be open before calling.
 *
 * Return: 0 on success, negative errno on failure.
 */
int cci_util_lt7911_reg_write(struct cci_util_handle *handle,
			      struct dpin_cci_util_i2c_reg_array *reg_cfg, int reg_sz)
{
	int rc;
	struct cci_util_dev *dev;

	if (!handle || !handle->dev)
		return -EINVAL;

	mutex_lock(&g_cci_util_lock);
	dev = handle->dev;

	rc = cci_ensure_configured(dev);
	if (rc < 0) {
		mutex_unlock(&g_cci_util_lock);
		return rc;
	}

	rc = lt7911_write_regs(&dev->client, reg_cfg, reg_sz);
	mutex_unlock(&g_cci_util_lock);
	return rc;
}
EXPORT_SYMBOL_GPL(cci_util_lt7911_reg_write);

/**
 * cci_util_lt7911_reg_read_seq - read multiple consecutive bytes from a register.
 * @handle:    opaque device handle obtained from cci_util_lt7911_get_device()
 * @reg:       starting register address (8-bit)
 * @out_buf:   caller-allocated buffer of at least @num_bytes bytes
 * @num_bytes: number of consecutive bytes to read (1-32)
 *
 * Acquires g_cci_util_lock, performs a sequential CCI read, then releases
 * the lock.  The I2C gate must already be open before calling.
 *
 * Return: 0 on success, negative errno on failure.
 */
int cci_util_lt7911_reg_read_seq(struct cci_util_handle *handle,
				 u8 reg, u8 *out_buf, u16 num_bytes)
{
	int rc;
	struct cci_util_dev *dev;

	if (!handle || !handle->dev || !out_buf || num_bytes == 0 || num_bytes > 32)
		return -EINVAL;

	mutex_lock(&g_cci_util_lock);
	dev = handle->dev;

	rc = cci_ensure_configured(dev);
	if (rc < 0) {
		mutex_unlock(&g_cci_util_lock);
		return rc;
	}

	rc = dpin_cci_util_read_seq(&dev->client, reg, out_buf, num_bytes);
	mutex_unlock(&g_cci_util_lock);

	if (rc < 0)
		dev_err(&dev->ppdev->dev,
			"[dpin_cci_util] reg_read_seq failed reg=0x%02x len=%u rc=%d\n",
			reg, num_bytes, rc);
	return rc;
}
EXPORT_SYMBOL_GPL(cci_util_lt7911_reg_read_seq);

static int32_t cci_util_probe(struct platform_device *pdev)
{
	struct cci_util_dev *dpbdev;
	struct device_node *np = pdev->dev.of_node;
	u32 val;
	int rc = 0;

	dpbdev = kzalloc(sizeof(*dpbdev), GFP_KERNEL);
	if (!dpbdev)
		return -ENOMEM;

	/* Get parent device ie cci device */
	dpbdev->ppdev = to_platform_device(pdev->dev.parent);
	dev_dbg(&pdev->dev, "%s pdev=%s parent=%s\n",
		 __func__, dev_name(&pdev->dev),
		 pdev->dev.parent ? dev_name(pdev->dev.parent) : "NULL");

	/* Read CCI configuration from Device Tree, fall back to defaults. */
	dpbdev->cci_dev_index = of_property_read_u32(np, "cci-device", &val)
		? CCI_DEV_INDEX_DEFAULT : (int)val;
	dpbdev->cci_master_id = of_property_read_u32(np, "cci-master", &val)
		? CCI_MASTER_ID_DEFAULT : (int)val;
	dpbdev->slave_addr    = of_property_read_u32(np, "reg", &val)
		? SLAVE_ADDR_DEFAULT    : (int)val;

	dev_dbg(&pdev->dev, "%s cci_dev=%d master=%d slave=0x%02x\n",
		 __func__, dpbdev->cci_dev_index,
		 dpbdev->cci_master_id, dpbdev->slave_addr);

	dpbdev->configured = false;

	mutex_lock(&g_cci_util_lock);
	g_cci_util = dpbdev;
	g_cci_dev  = &pdev->dev;
	mutex_unlock(&g_cci_util_lock);

	return rc;
}

static int cci_util_remove(struct platform_device *pdev)
{
	struct cci_util_dev *dpbdev;

	mutex_lock(&g_cci_util_lock);
	dpbdev     = g_cci_util;
	g_cci_util = NULL;
	g_cci_dev  = NULL;
	mutex_unlock(&g_cci_util_lock);

	if (dpbdev) {
		if (dpbdev->configured) {
			cci_util_release(dpbdev);
			dpbdev->configured = false;
		}
		kfree(dpbdev);
	}
	return 0;
}

static int cci_util_suspend(struct device *dev)
{
	struct cci_util_dev *dpbdev;

	mutex_lock(&g_cci_util_lock);
	dpbdev = g_cci_util;
	if (dpbdev && dpbdev->configured) {
		cci_util_release(dpbdev);
		dpbdev->configured = false;
	}
	mutex_unlock(&g_cci_util_lock);

	return 0;
}

static int cci_util_resume(struct device *dev)
{
	return 0;
}

static const struct dev_pm_ops cci_util_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(cci_util_suspend, cci_util_resume)
		SET_RUNTIME_PM_OPS(cci_util_suspend, cci_util_resume, NULL)
};

static const struct of_device_id cci_util_dt_match[] = {
	{ .compatible = "lontium,lt7911uxc_cci_util" },
	{}
};

MODULE_DEVICE_TABLE(of, cci_util_dt_match);

static struct platform_driver cci_util_platform_driver = {
	.probe = cci_util_probe,
	.driver = {
		.name = "lt7911uxc_cci_util",
		.of_match_table = cci_util_dt_match,
		.pm = &cci_util_pm_ops,
		.suppress_bind_attrs = true,
	},
	.remove = cci_util_remove,
};
module_platform_driver(cci_util_platform_driver);

MODULE_DESCRIPTION("Util driver to access LT7911UXC CCI peripherals");
MODULE_LICENSE("GPL");
