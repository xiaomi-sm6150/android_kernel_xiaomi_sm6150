// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * All rights reserved.
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/kobject.h>
#include "usbmux_ps8822.h"

static const char * const mode_names[] = {
	[USBMUX_MODE_SAFE] = "safe_mode",
	[USBMUX_MODE_USB3_ONLY] = "usb3_only",
	[USBMUX_MODE_DP4_LANE] = "dp4_lane",
	[USBMUX_MODE_USB3_2LANE_DP2] = "usb3_2lane_dp2",
};

/**
 * struct usbmux_handle - opaque handle handed to clients.
 *
 * Wraps a pointer back to the owning usbmux_ps8822_data so that all exported
 * functions can reach private driver state without touching any global
 * variable.  The struct is forward-declared in the public header; its
 * layout is private to this file.
 */
struct usbmux_handle {
	struct usbmux_ps8822_data *data;
};

static struct usbmux_ps8822_data *s_usbmux_data;
static DEFINE_MUTEX(s_usbmux_lock);

/**
 * usbmux_get_device - obtain a handle to the usbmux_ps8822 device.
 *
 * Acquires s_usbmux_lock to safely read s_usbmux_data.  Returns NULL when
 * the driver has not yet probed or has already been removed, giving the
 * caller a natural "not ready" signal before any other API is called.
 * The caller must release the handle with usbmux_put_device().
 */
struct usbmux_handle *usbmux_get_device(void)
{
	struct usbmux_handle *handle;

	mutex_lock(&s_usbmux_lock);
	if (!s_usbmux_data) {
		mutex_unlock(&s_usbmux_lock);
		return NULL;
	}

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (handle)
		handle->data = s_usbmux_data;
	mutex_unlock(&s_usbmux_lock);
	return handle;
}
EXPORT_SYMBOL_GPL(usbmux_get_device);

/**
 * usbmux_put_device - release a handle obtained from usbmux_get_device.
 * @handle: handle to release (NULL is silently ignored)
 */
void usbmux_put_device(struct usbmux_handle *handle)
{
	kfree(handle);
}
EXPORT_SYMBOL_GPL(usbmux_put_device);

/**
 * usbmux_write_reg - Write PS8822 register via I2C
 * @data: Device data
 * @page_addr: Page address (0x20/0x22/0x24)
 * @reg: Register offset
 * @val: Value to write
 *
 * Return: 0 on success, negative error code on failure
 */
static int usbmux_write_reg(struct usbmux_ps8822_data *data, u8 page_addr, u8 reg, u8 val)
{
	struct i2c_msg msg = {};
	u8 buf[I2C_WRITE_MSG_LEN] = {};
	int ret = 0;

	if (!data || !data->client || !data->client->adapter)
		return -EINVAL;

	buf[0] = reg;
	buf[1] = val;

	msg.addr = page_addr >> 1;  /* Convert to 7-bit address */
	msg.flags = 0;	/* Write */
	msg.len = I2C_WRITE_MSG_LEN;
	msg.buf = buf;

	ret = i2c_transfer(data->client->adapter, &msg, I2C_WRITE_MSG_COUNT);
	if (ret != I2C_WRITE_MSG_COUNT) {
		dev_err(data->dev, "I2C write failed: page=0x%02x reg=0x%02x val=0x%02x ret=%d\n",
			 page_addr, reg, val, ret);
		return ret < 0 ? ret : -EIO;
	}

	return 0;
}

/**
 * usbmux_read_reg - Read PS8822 register via I2C
 * @data: Device data
 * @page_addr: Page address (0x20/0x22/0x24)
 * @reg: Register offset
 * @val: Pointer to store read value
 *
 * Return: 0 on success, negative error code on failure
 */
static int usbmux_read_reg(struct usbmux_ps8822_data *data, u8 page_addr, u8 reg, u8 *val)
{
	struct i2c_msg msgs[2] = {};
	u8 reg_addr_buf = reg;
	int ret = 0;

	if (!data || !data->client || !data->client->adapter || !val)
		return -EINVAL;

	msgs[0].addr = page_addr >> 1;	/* Convert to 7-bit address */
	msgs[0].flags = 0;  /* Write */
	msgs[0].len = I2C_ADDR_MSG_LEN;
	msgs[0].buf = &reg_addr_buf;

	msgs[1].addr = page_addr >> 1;
	msgs[1].flags = I2C_M_RD;  /* Read */
	msgs[1].len = I2C_DATA_MSG_LEN;
	msgs[1].buf = val;

	ret = i2c_transfer(data->client->adapter, msgs, I2C_READ_MSG_COUNT);
	if (ret != I2C_READ_MSG_COUNT) {
		dev_err(data->dev, "I2C read failed: page=0x%02x reg=0x%02x ret=%d\n",
			 page_addr, reg, ret);
		return ret < 0 ? ret : -EIO;
	}

	return 0;
}

static int usbmux_set_mode(struct usbmux_ps8822_data *data, enum usbmux_mode mode,
			   int orientation)
{
	int ret = 0;
	u8 mode_reg = 0;

	if (!data)
		return -EINVAL;

	if (mode >= USBMUX_MODE_MAX) {
		dev_err(data->dev, "Invalid mode: %d\n", mode);
		return -EINVAL;
	}

	mutex_lock(&data->lock);

	/*
	 * Update orientation under data->lock so it is always consistent
	 * with the mode register write that follows.  A value of -1 means
	 * "keep the current orientation" (used by usbmux_hw_init).
	 */
	if (orientation == 0)
		data->orientation_flip = false;
	else if (orientation == 1)
		data->orientation_flip = true;
	/* else orientation == -1: leave data->orientation_flip unchanged */

	switch (mode) {
	case USBMUX_MODE_SAFE:
		mode_reg = MODE_BASE_SAFE;
		break;
	case USBMUX_MODE_USB3_ONLY:
		mode_reg = MODE_BASE_USB_ONLY;
		break;
	case USBMUX_MODE_DP4_LANE:
		/* Use 4-lane DP Mode C as default */
		mode_reg = MODE_BASE_4LANE_DP_C;
		break;
	case USBMUX_MODE_USB3_2LANE_DP2:
		mode_reg = MODE_BASE_USB_2LANE_DP;  /* 1-port USB + 2-lane DP */
		break;
	default:
		ret = -EINVAL;
		goto unlock;
	}

	/* Apply orientation flip bit if needed (Bit 5) */
	if (data->orientation_flip)
		mode_reg |= MODE_CTRL_FLIP_ENABLE;

	/* Write to Page0.0x01 register */
	ret = usbmux_write_reg(data, PS8822_PAGE0_ADDR,
			       PS8822_P0_MODE_CTRL, mode_reg);
	if (ret) {
		dev_err(data->dev, "Failed to set mode: %d\n", ret);
		goto unlock;
	}

	/* Wait for mode switch to complete */
	usleep_range(MODE_SWITCH_DELAY_MIN, MODE_SWITCH_DELAY_MAX);

	data->current_mode = mode;
	dev_dbg(data->dev, "Mode changed to: %s (reg=0x%02x)\n", mode_names[mode], mode_reg);

unlock:
	mutex_unlock(&data->lock);
	return ret;
}

void usbmux_setmode(struct usbmux_handle *handle, int lanes, int orientation)
{
	struct usbmux_ps8822_data *data;

	if (!handle || !handle->data)
		return;

	data = handle->data;

	if (orientation != 0 && orientation != 1) {
		dev_err(data->dev, "Invalid orientation %d, must be 0 (normal) or 1 (flipped)\n",
			   orientation);
		return;
	}

	dev_dbg(data->dev, "lanes:%d orientation:%d\n", lanes, orientation);

	switch (lanes) {
	case 0:
		usbmux_set_mode(data, USBMUX_MODE_USB3_ONLY, orientation);
		break;
	case 2:
		usbmux_set_mode(data, USBMUX_MODE_USB3_2LANE_DP2, orientation);
		break;
	case 4:
		usbmux_set_mode(data, USBMUX_MODE_DP4_LANE, orientation);
		break;
	default:
		dev_err(data->dev, "Invalid lane count %d, must be 0, 2, or 4\n", lanes);
		break;
	}
}
EXPORT_SYMBOL_GPL(usbmux_setmode);

static int usbmux_send_sinkonly(struct device *dev, bool enable)
{
	dev_err(dev, "ucsi_glink not available - SET_SINKONLY not sent (enable=%d)\n",
			enable);
	return -EOPNOTSUPP;
}

static int usbmux_send_cc_reconnect(struct device *dev)
{
	dev_err(dev, "ucsi_glink not available - CC_RECONNECT not sent\n");
	return -EOPNOTSUPP;
}

/**
 * usbmux_set_dp_state - Set DP state with sink-only mode control
 * @data: Device data pointer
 * @enable: true to enable DP state (sink-only), false to disable
 *
 * Common routine to handle DP state changes by sending UCSI commands
 * to ADSP for sink-only mode control and CC reconnection.
 *
 * Return: 0 on success, negative error code on failure
 */
static int usbmux_set_dp_state(struct usbmux_ps8822_data *data, bool enable)
{
	int ret = 0;

	if (!data)
		return -EINVAL;

	ret = usbmux_send_sinkonly(data->dev, enable);
	if (ret < 0) {
		dev_err(data->dev, "Failed to send sinkonly command: %d\n", ret);
		return ret;
	}

	data->dp_state = enable;
	dev_dbg(data->dev, "dp_state changed to %d\n", enable);

	ret = usbmux_send_cc_reconnect(data->dev);
	if (ret < 0) {
		dev_err(data->dev, "CC_RECONNECT failed: %d (dp_state already %d)\n",
			 ret, enable);
		return ret;
	}

	return 0;
}

static ssize_t dp_state_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct usbmux_ps8822_data *data = NULL;

	data = dev_get_drvdata(dev);
	if (!data)
		return -EINVAL;

	return scnprintf(buf, PAGE_SIZE, "%d\n", data->dp_state ? 1 : 0);
}

static ssize_t dp_state_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct usbmux_ps8822_data *data = NULL;
	int val = 0;
	int ret = 0;
	bool new_dp_state = false;

	data = dev_get_drvdata(dev);
	if (!data)
		return -EINVAL;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	new_dp_state = (val != 0);

	ret = usbmux_set_dp_state(data, new_dp_state);
	if (ret < 0)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(dp_state);

static struct attribute *usbmux_ps8822_attrs[] = {
	&dev_attr_dp_state.attr,
	NULL
};

static const struct attribute_group usbmux_ps8822_attr_group = {
	.attrs = usbmux_ps8822_attrs,
};

static int usbmux_set_hpd(struct usbmux_ps8822_data *data, bool enable)
{
	int ret = 0;
	u8 hpd_reg = 0;

	if (!data)
		return -EINVAL;

	mutex_lock(&data->lock);

	hpd_reg = HPD_CTRL_DISABLE_PIN;
	if (enable)
		hpd_reg |= HPD_CTRL_STATUS;

	/* Write to Page0.0x02 register */
	ret = usbmux_write_reg(data, PS8822_PAGE0_ADDR,
					PS8822_P0_HPD_CTRL, hpd_reg);
	if (ret) {
		dev_err(data->dev, "Failed to set HPD: %d\n", ret);
		goto unlock;
	}

	if (data->hpd_gpio)
		gpiod_set_value_cansleep(data->hpd_gpio, enable ? 1 : 0);

	data->hpd_state = enable;
	dev_dbg(data->dev, "HPD: %s\n", enable ? "asserted" : "deasserted");

unlock:
	mutex_unlock(&data->lock);

	return ret;
}

void usbmux_sethpd(struct usbmux_handle *handle, bool enable)
{
	if (!handle || !handle->data)
		return;
	usbmux_set_hpd(handle->data, enable);
}
EXPORT_SYMBOL_GPL(usbmux_sethpd);

int usbmux_setdpstate(struct usbmux_handle *handle, bool enable)
{
	if (!handle || !handle->data)
		return -ENODEV;

	return usbmux_set_dp_state(handle->data, enable);
}
EXPORT_SYMBOL_GPL(usbmux_setdpstate);

static int usbmux_read_data_id(struct usbmux_ps8822_data *data, char *buf, size_t len)
{
	u8 id[CHIP_ID_BYTES] = {};
	int i = 0, ret = 0;

	if (!data || !buf)
		return -EINVAL;

	if (len < CHIP_ID_STR_LEN)  /* Need at least 7 bytes for "PS8822\0" */
		return -EINVAL;

	mutex_lock(&data->lock);

	/* Read data ID bytes (should be "PS8822") */
	for (i = 0; i < CHIP_ID_BYTES; i++) {
		ret = usbmux_read_reg(data, PS8822_PAGE0_ADDR,
				      PS8822_P0_CHIP_ID0 + i, &id[i]);
		if (ret) {
			dev_err(data->dev, "Failed to read data ID byte %d: %d\n", i, ret);
			goto unlock;
		}
	}

	snprintf(buf, len, CHIP_ID_FORMAT, id[0], id[1], id[2], id[3], id[4], id[5]);
	ret = 0;

unlock:
	mutex_unlock(&data->lock);
	return ret;
}

/* Hardware initialization */
static int usbmux_hw_init(struct usbmux_ps8822_data *data)
{
	char chip_id[CHIP_ID_STR_LEN] = {};
	int ret = 0;

	if (!data)
		return -EINVAL;

	/* Power on sequence: switch -> vdd1v2 -> vdd3v3 -> enable */
	/* Step 0: Enable switch first */
	if (data->switch_gpio)
		gpiod_set_value_cansleep(data->switch_gpio, 1);

	/* Step 1: Power on 1.2V rail */
	if (data->vdd1v2_gpio)
		gpiod_set_value_cansleep(data->vdd1v2_gpio, 1);

	/* Step 2: Power on 3.3V rail */
	if (data->vdd3v3_gpio)
		gpiod_set_value_cansleep(data->vdd3v3_gpio, 1);

	/* Step 3: Enable the data */
	if (data->enable_gpio) {
		gpiod_set_value_cansleep(data->enable_gpio, 1);
		msleep(CHIP_INIT_DELAY_MS);  /* Wait for data initialization and I2C ready */
	}

	ret = usbmux_read_data_id(data, chip_id, sizeof(chip_id));
	if (ret) {
		dev_warn(data->dev, "Failed to read data ID: %d (continuing for debug)\n", ret);
		/* Don't fail init - allow GPIO debug */
	} else {
		dev_dbg(data->dev, "Chip ID: %s\n", chip_id);
	}

	ret = usbmux_set_mode(data, data->current_mode, -1);
	if (ret) {
		dev_warn(data->dev, "Failed to set default mode: %d (continuing)\n", ret);
		/* Don't fail init - allow GPIO debug */
	}

	return 0;
}

static int usbmux_parse_dt(struct device *dev, struct usbmux_ps8822_data *data)
{
	u32 val = 0;

	if (!data || !dev)
		return -EINVAL;

	if (!dev->of_node)
		return -ENODEV;

	if (!of_property_read_u32(dev->of_node, "usbmux,default-mode", &val)) {
		if (val < USBMUX_MODE_MAX) {
			data->current_mode = val;
			dev_dbg(dev, "Default mode from DT: %s\n", mode_names[val]);
		} else {
			dev_warn(dev, "Invalid default mode %d, using USB3_2LANE_DP2\n", val);
			data->current_mode = USBMUX_MODE_USB3_2LANE_DP2;
		}
	} else {
		/* Default to 2-lane USB3 + 2-lane DP if not specified */
		data->current_mode = USBMUX_MODE_USB3_2LANE_DP2;
		dev_dbg(dev, "Using default mode: USB3_2LANE_DP2\n");
	}

	if (!of_property_read_u32(dev->of_node, "usbmux,orientation-normal", &val)) {
		data->orientation_flip = (val == 0) ? false : true;
		dev_dbg(dev, "Default orientation from DT: %s\n",
			 data->orientation_flip ? "flipped" : "normal");
	} else {
		/* Default to normal orientation if not specified */
		data->orientation_flip = false;
		dev_dbg(dev, "Using default orientation: normal\n");
	}

	data->switch_gpio = devm_gpiod_get_optional(dev, "switch", GPIOD_OUT_LOW);
	if (IS_ERR(data->switch_gpio)) {
		dev_err(dev, "Failed to request switch GPIO: %ld\n",
			   PTR_ERR(data->switch_gpio));
		return PTR_ERR(data->switch_gpio);
	}
	if (!data->switch_gpio)
		dev_dbg(dev, "Switch GPIO not defined\n");

	data->vdd1v2_gpio = devm_gpiod_get_optional(dev, "vdd1v2", GPIOD_OUT_LOW);
	if (IS_ERR(data->vdd1v2_gpio)) {
		dev_err(dev, "Failed to request vdd1v2 GPIO: %ld\n",
			   PTR_ERR(data->vdd1v2_gpio));
		return PTR_ERR(data->vdd1v2_gpio);
	}
	if (!data->vdd1v2_gpio)
		dev_dbg(dev, "VDD1V2 GPIO not defined\n");

	data->vdd3v3_gpio = devm_gpiod_get_optional(dev, "vdd3v3", GPIOD_OUT_LOW);
	if (IS_ERR(data->vdd3v3_gpio)) {
		dev_err(dev, "Failed to request vdd3v3 GPIO: %ld\n",
			   PTR_ERR(data->vdd3v3_gpio));
		return PTR_ERR(data->vdd3v3_gpio);
	}
	if (!data->vdd3v3_gpio)
		dev_dbg(dev, "VDD3V3 GPIO not defined\n");

	data->enable_gpio = devm_gpiod_get_optional(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(data->enable_gpio)) {
		dev_err(dev, "Failed to request enable GPIO: %ld\n",
			   PTR_ERR(data->enable_gpio));
		return PTR_ERR(data->enable_gpio);
	}
	if (!data->enable_gpio)
		dev_dbg(dev, "Enable GPIO not defined\n");

	data->hpd_gpio = devm_gpiod_get_optional(dev, "hpd", GPIOD_OUT_LOW);
	if (IS_ERR(data->hpd_gpio)) {
		dev_err(dev, "Failed to request hpd GPIO: %ld\n",
			   PTR_ERR(data->hpd_gpio));
		return PTR_ERR(data->hpd_gpio);
	}
	if (!data->hpd_gpio)
		dev_dbg(dev, "HPD GPIO not defined\n");

	return 0;
}

static int usbmux_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct usbmux_ps8822_data *data;
	int ret = 0;

	data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	ret = devm_mutex_init(&client->dev, &data->lock);
	if (ret)
		return ret;

	data->client = client;
	data->dev = &client->dev;
	i2c_set_clientdata(client, data);

	ret = usbmux_parse_dt(&client->dev, data);
	if (ret) {
		dev_err(&client->dev, "Failed to parse device tree: %d\n", ret);
		return ret;
	}

	ret = usbmux_hw_init(data);
	if (ret) {
		dev_err(&client->dev, "Failed to initialize HW: %d\n", ret);
		return ret;
	}

	data->dp_state = false;

	ret = devm_device_add_group(&client->dev, &usbmux_ps8822_attr_group);
	if (ret) {
		dev_err(&client->dev, "Failed to create sysfs group: %d\n", ret);
		return ret;
	}

	mutex_lock(&s_usbmux_lock);
	s_usbmux_data = data;
	mutex_unlock(&s_usbmux_lock);

	dev_dbg(&client->dev, "usbmux_ps8822 probed successfully\n");
	return 0;
}

static const struct of_device_id usbmux_ps8822_of_match[] = {
	{ .compatible = "parade,usbmux-ps8822", },
	{ }
};
MODULE_DEVICE_TABLE(of, usbmux_ps8822_of_match);

static void usbmux_remove(struct i2c_client *client)
{
	struct usbmux_ps8822_data *data = i2c_get_clientdata(client);

	mutex_lock(&s_usbmux_lock);
	s_usbmux_data = NULL;
	mutex_unlock(&s_usbmux_lock);

	if (data) {
		usbmux_set_hpd(data, false);
		usbmux_set_mode(data, USBMUX_MODE_SAFE, -1);
		if (data->enable_gpio)
			gpiod_set_value_cansleep(data->enable_gpio, 0);
		if (data->vdd3v3_gpio)
			gpiod_set_value_cansleep(data->vdd3v3_gpio, 0);
		if (data->vdd1v2_gpio)
			gpiod_set_value_cansleep(data->vdd1v2_gpio, 0);
		if (data->switch_gpio)
			gpiod_set_value_cansleep(data->switch_gpio, 0);
	}
}

static struct i2c_driver usbmux_driver = {
	.driver = {
		.name = "usbmux_ps8822",
		.of_match_table = usbmux_ps8822_of_match,
	},
	.probe  = usbmux_probe,
	.remove = usbmux_remove,
};

module_i2c_driver(usbmux_driver);

MODULE_DESCRIPTION("DP/USB Mux Manager for PS8822");
MODULE_LICENSE("GPL");
