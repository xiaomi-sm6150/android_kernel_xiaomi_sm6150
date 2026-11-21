// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The device control driver for Fortsense's fingerprint sensor.
 *
 * Copyright (C) 2018 Fortsense Corporation. <http://www.fortsense.com>
 * Copyright (C) 2021 XiaoMi, Inc.
 *
 * 6.1 port of the 4.14 sweet driver (sf_ctl.c + sf_hw.c, v2.4.2-2020-11-09)
 * with only the configuration sweet built: SF_TEE_QSEE + SF_COMPATIBLE_QSEE,
 * VDD from a regulator, device registered by the driver. The sensor sits on
 * SPI owned by the TEE; this driver only powers and resets it and forwards
 * its IRQ to the HAL as a uevent.
 *
 * Nothing is claimed at probe ("multi HAL compatible"): the HAL requests the
 * GPIOs, regulator and IRQ through SF_IOC_INIT_DRIVER / SF_IOC_REQUEST_IRQ,
 * so the driver coexists with the FPC driver on the same pins.
 *
 * Userspace ABI kept from 4.14: /dev/fortsense_fp and its ioctls, the
 * version/chip_info/tee_version/set_fun attributes on the misc device, the
 * "SPI_STATE=finger" and "SCREEN_STATUS=ON|OFF" uevents and the
 * "uinput-fortsense" input device.
 */

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/regulator/consumer.h>
#include <linux/soc/qcom/panel_event_notifier.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <drm/drm_panel.h>

#define SF_DRV_VERSION		"v2.4.2-2020-11-09"
#define SF_DRV_VERSION_LEN	32

#define COMPATIBLE_SW_FP	"fs,fingerprint"
#define COMPATIBLE_RESET_GPIO	"fs,gpio-reset"
#define COMPATIBLE_IRQ_GPIO	"fs,gpio-irq"
#define SF_VDD_NAME		"fs_vdd"

#define SF_INT_EVENT_NAME	"SPI_STATE=finger"

#define XIAOMI_KEYCODE_DOUBLE_CLK 306	/* 0x132 */
#define XIAOMI_KEYCODE_SINGLE_CLK 353	/* 0x161 */

#define xprintk(level, fmt, args...) \
	printk(level "fortsense-sf_ctl: (%d) " fmt, __LINE__, ##args)

typedef enum {
	SF_KEY_NONE = 0,
	SF_KEY_HOME,
	SF_KEY_MENU,
	SF_KEY_BACK,
	SF_KEY_F11,
	SF_KEY_ENTER,
	SF_KEY_UP,
	SF_KEY_LEFT,
	SF_KEY_RIGHT,
	SF_KEY_DOWN,
	SF_KEY_WAKEUP,
	SF_KEY_F12,
} sf_key_type_t;

typedef struct {
	sf_key_type_t key;
	int value;	/* for key type, 0 means up, 1 means down. */
} sf_key_event_t;

#define SF_MAX_VER_INFO_LEN 32

typedef struct {
	char tee_solution[SF_MAX_VER_INFO_LEN];
	char ca_version[SF_MAX_VER_INFO_LEN * 2];
	char ta_version[SF_MAX_VER_INFO_LEN * 2];
	char algorithm[SF_MAX_VER_INFO_LEN];
	char algo_nav[SF_MAX_VER_INFO_LEN];
	char driver[SF_MAX_VER_INFO_LEN];
	char firmware[SF_MAX_VER_INFO_LEN];
	char fortsense_id[SF_MAX_VER_INFO_LEN];
	char vendor_id[SF_MAX_VER_INFO_LEN];
} __attribute__((__packed__)) sf_version_info_t;

/* Magic code for IOCTL-subsystem, 's'(0x73) means 'Fortsense'. */
#define SF_IOC_MAGIC 's'
#define SF_IOC_INIT_DRIVER	_IO(SF_IOC_MAGIC, 0x00)
#define SF_IOC_DEINIT_DRIVER	_IO(SF_IOC_MAGIC, 0x01)
#define SF_IOC_RESET_DEVICE	_IO(SF_IOC_MAGIC, 0x02)
#define SF_IOC_ENABLE_IRQ	_IO(SF_IOC_MAGIC, 0x03)
#define SF_IOC_DISABLE_IRQ	_IO(SF_IOC_MAGIC, 0x04)
#define SF_IOC_REQUEST_IRQ	_IO(SF_IOC_MAGIC, 0x21)
#define SF_IOC_ENABLE_SPI_CLK	_IO(SF_IOC_MAGIC, 0x05)
#define SF_IOC_DISABLE_SPI_CLK	_IO(SF_IOC_MAGIC, 0x06)
#define SF_IOC_ENABLE_POWER	_IO(SF_IOC_MAGIC, 0x07)
#define SF_IOC_DISABLE_POWER	_IO(SF_IOC_MAGIC, 0x08)
#define SF_IOC_REPORT_KEY_EVENT	_IOW(SF_IOC_MAGIC, 0x09, sf_key_event_t *)
#define SF_IOC_SYNC_CONFIG	_IOWR(SF_IOC_MAGIC, 0x0a, void *)
#define SF_IOC_SPI_SPEED	_IO(SF_IOC_MAGIC, 0x0b)
#define SF_IOC_ATTRIBUTE	_IO(SF_IOC_MAGIC, 0x0d)
#define SF_IOC_GET_VERSION	_IOR(SF_IOC_MAGIC, 0x20, const char *)
#define SF_IOC_SET_LIB_VERSION	_IO(SF_IOC_MAGIC, 0x30)
#define SF_IOC_GET_LIB_VERSION	_IO(SF_IOC_MAGIC, 0x31)
#define SF_IOC_SET_SPI_BUF_SIZE	_IO(SF_IOC_MAGIC, 0x40)
#define SF_IOC_SET_RESET_OUTPUT	_IO(SF_IOC_MAGIC, 0x50)

/* REE emulate TEE */
#define SPI_IOC_MAGIC			'k'
#define SPI_IOC_RD_MAX_SPEED_HZ		_IOR(SPI_IOC_MAGIC, 4, __u32)
#define SPI_IOC_WR_MAX_SPEED_HZ		_IOW(SPI_IOC_MAGIC, 4, __u32)
#define SPI_IOC_RST			_IO(SPI_IOC_MAGIC, 5)
#define FORTSENSE_IOC_ATTRIBUTE		_IOW(SPI_IOC_MAGIC, 17, __u32)

struct sf_ctl_device {
	struct miscdevice miscdev;
	struct platform_device *pdev;
	struct device_node *np;
	struct mutex lock;		/* GPIO/regulator/IRQ claim state */
	int rst_num;
	int irq_pin;
	int irq_num;
	bool gpios_requested;
	bool irq_requested;
	bool powered;
	struct regulator *vdd_reg;
	struct work_struct work_queue;
	struct input_dev *input;
	struct wakeup_source *wakelock;
	void *panel_cookie;
	int attribute;
	int spi_buf_size;
};

static struct sf_ctl_device sf_ctl_dev = {
	.rst_num = -EINVAL,
	.irq_pin = -EINVAL,
	.irq_num = -EINVAL,
	.spi_buf_size = 25 * 1024,
};

static struct platform_device *sf_device;
static sf_version_info_t sf_hw_ver;

/* ------------------------------------------------------------------------ */
/* hardware: reset, power, GPIO claim (sf_hw.c) */

static int sf_ctl_device_reset(bool output)
{
	if (!gpio_is_valid(sf_ctl_dev.rst_num)) {
		xprintk(KERN_ERR, "reset gpio not requested.\n");
		return -ENODEV;
	}

	return gpio_direction_output(sf_ctl_dev.rst_num, output ? 1 : 0);
}

static int sf_ctl_device_power(bool on)
{
	int err = 0;

	if (!sf_ctl_dev.vdd_reg) {
		xprintk(KERN_ERR, "vdd regulator not requested.\n");
		return -ENODEV;
	}

	if (on && !sf_ctl_dev.powered) {
		err = regulator_set_load(sf_ctl_dev.vdd_reg, 100000);
		if (err < 0)
			xprintk(KERN_ERR, "Regulator set_load failed err = %d\n", err);

		err = regulator_enable(sf_ctl_dev.vdd_reg);
		if (err) {
			xprintk(KERN_ERR, "Regulator vdd enable failed err = %d\n", err);
			return err;
		}
		sf_ctl_dev.powered = true;
		msleep(10);
	} else if (!on && sf_ctl_dev.powered) {
		err = regulator_disable(sf_ctl_dev.vdd_reg);
		if (err) {
			xprintk(KERN_ERR, "Regulator vdd disable failed err = %d\n", err);
			return err;
		}
		sf_ctl_dev.powered = false;
	}

	return err;
}

static void sf_ctl_free_irq(void)
{
	if (sf_ctl_dev.irq_requested) {
		disable_irq_wake(sf_ctl_dev.irq_num);
		free_irq(sf_ctl_dev.irq_num, &sf_ctl_dev);
		sf_ctl_dev.irq_requested = false;
	}
}

static void sf_ctl_device_free_gpio(void)
{
	xprintk(KERN_INFO, "%s(..) enter, free resource.\n", __func__);

	sf_ctl_free_irq();

	if (gpio_is_valid(sf_ctl_dev.rst_num)) {
		gpio_free(sf_ctl_dev.rst_num);
		sf_ctl_dev.rst_num = -EINVAL;
	}
	if (gpio_is_valid(sf_ctl_dev.irq_pin)) {
		gpio_free(sf_ctl_dev.irq_pin);
		sf_ctl_dev.irq_pin = -EINVAL;
	}
	sf_ctl_dev.irq_num = -EINVAL;

	if (sf_ctl_dev.vdd_reg) {
		sf_ctl_device_power(false);
		regulator_put(sf_ctl_dev.vdd_reg);
		sf_ctl_dev.vdd_reg = NULL;
	}

	sf_ctl_dev.gpios_requested = false;
}

static void sf_ctl_register_panel(void);

static int sf_ctl_device_init_gpio_pins(void)
{
	struct device_node *np = sf_ctl_dev.np;
	int err;

	if (sf_ctl_dev.gpios_requested)
		return 0;

	if (!np) {
		xprintk(KERN_ERR, "no '%s' node.\n", COMPATIBLE_SW_FP);
		return -ENODEV;
	}

	sf_ctl_dev.rst_num = of_get_named_gpio(np, COMPATIBLE_RESET_GPIO, 0);
	sf_ctl_dev.irq_pin = of_get_named_gpio(np, COMPATIBLE_IRQ_GPIO, 0);
	xprintk(KERN_INFO, "reset pin number = %d\n", sf_ctl_dev.rst_num);
	xprintk(KERN_INFO, "irq pin number = %d\n", sf_ctl_dev.irq_pin);

	if (!gpio_is_valid(sf_ctl_dev.rst_num)) {
		xprintk(KERN_ERR, "not valid reset gpio\n");
		err = -EIO;
		goto fail;
	}
	err = gpio_request(sf_ctl_dev.rst_num, "sf-reset");
	if (err) {
		xprintk(KERN_ERR, "Could not request reset gpio.\n");
		sf_ctl_dev.rst_num = -EINVAL;
		goto fail;
	}

	if (!gpio_is_valid(sf_ctl_dev.irq_pin)) {
		xprintk(KERN_ERR, "not valid irq gpio\n");
		err = -EIO;
		goto fail;
	}
	err = gpio_request(sf_ctl_dev.irq_pin, "sf-irq");
	if (err) {
		xprintk(KERN_ERR, "Could not request irq gpio.\n");
		sf_ctl_dev.irq_pin = -EINVAL;
		goto fail;
	}
	gpio_direction_input(sf_ctl_dev.irq_pin);
	sf_ctl_dev.irq_num = gpio_to_irq(sf_ctl_dev.irq_pin);
	xprintk(KERN_INFO, "irq number = %d\n", sf_ctl_dev.irq_num);

	sf_ctl_dev.vdd_reg = regulator_get(&sf_ctl_dev.pdev->dev, SF_VDD_NAME);
	if (IS_ERR(sf_ctl_dev.vdd_reg)) {
		err = PTR_ERR(sf_ctl_dev.vdd_reg);
		sf_ctl_dev.vdd_reg = NULL;
		xprintk(KERN_ERR, "Regulator get failed vdd err = %d\n", err);
		goto fail;
	}

	sf_ctl_dev.gpios_requested = true;
	sf_ctl_device_power(true);
	sf_ctl_register_panel();
	return 0;

fail:
	sf_ctl_device_free_gpio();
	return err;
}

/* ------------------------------------------------------------------------ */
/* IRQ and screen state */

static void sf_ctl_device_event(struct work_struct *ws)
{
	char *uevent_env[2] = { SF_INT_EVENT_NAME, NULL };

	kobject_uevent_env(&sf_ctl_dev.miscdev.this_device->kobj,
			   KOBJ_CHANGE, uevent_env);
}

static irqreturn_t sf_ctl_device_irq(int irq, void *dev_id)
{
	disable_irq_nosync(irq);
	xprintk(KERN_INFO, "%s(irq = %d, ..) toggled.\n", __func__, irq);
	schedule_work(&sf_ctl_dev.work_queue);
	__pm_wakeup_event(sf_ctl_dev.wakelock, 5000);
	enable_irq(irq);
	return IRQ_HANDLED;
}

static int sf_ctl_init_irq(void)
{
	int err;

	if (sf_ctl_dev.irq_requested)
		return 0;
	if (sf_ctl_dev.irq_num < 0) {
		xprintk(KERN_ERR, "irq gpio not requested.\n");
		return -ENODEV;
	}

	err = request_irq(sf_ctl_dev.irq_num, sf_ctl_device_irq,
			  IRQF_TRIGGER_FALLING | IRQF_ONESHOT, "sf-irq",
			  &sf_ctl_dev);
	if (err) {
		xprintk(KERN_ERR, "request_irq(..) = %d.\n", err);
		return err;
	}

	enable_irq_wake(sf_ctl_dev.irq_num);
	sf_ctl_dev.irq_requested = true;
	return 0;
}

static void sf_ctl_screen_uevent(bool on)
{
	char *screen[2] = { on ? "SCREEN_STATUS=ON" : "SCREEN_STATUS=OFF", NULL };

	kobject_uevent_env(&sf_ctl_dev.miscdev.this_device->kobj,
			   KOBJ_CHANGE, screen);
}

/*
 * 4.14 sent SCREEN_STATUS from the fb notifier on FB_EVENT_BLANK, i.e. after
 * the blank/unblank. 6.1 has no fb events from the DSI display; take them from
 * the panel event notifier instead.
 */
static void sf_panel_event_cb(enum panel_event_notifier_tag tag,
		struct panel_event_notification *notification, void *client_data)
{
	if (!notification || notification->notif_data.early_trigger)
		return;

	if (notification->notif_type == DRM_PANEL_EVENT_UNBLANK)
		sf_ctl_screen_uevent(true);
	else if (notification->notif_type == DRM_PANEL_EVENT_BLANK)
		sf_ctl_screen_uevent(false);
}

/*
 * The node lists every panel sweet ships with; only the one the bootloader
 * picked registers a drm_panel. Called from SF_IOC_INIT_DRIVER, when the
 * display is long up.
 */
static void sf_ctl_register_panel(void)
{
	struct device_node *pnode;
	struct drm_panel *panel;
	int i, count;

	if (sf_ctl_dev.panel_cookie || !sf_ctl_dev.np)
		return;

	count = of_count_phandle_with_args(sf_ctl_dev.np, "panel", NULL);
	for (i = 0; i < count; i++) {
		pnode = of_parse_phandle(sf_ctl_dev.np, "panel", i);
		panel = of_drm_find_panel(pnode);
		of_node_put(pnode);
		if (IS_ERR(panel))
			continue;

		sf_ctl_dev.panel_cookie = panel_event_notifier_register(
			PANEL_EVENT_NOTIFICATION_PRIMARY,
			PANEL_EVENT_NOTIFIER_CLIENT_FINGERPRINT, panel,
			sf_panel_event_cb, &sf_ctl_dev);
		if (IS_ERR(sf_ctl_dev.panel_cookie))
			sf_ctl_dev.panel_cookie = NULL;
		return;
	}

	xprintk(KERN_ERR, "no active panel, no SCREEN_STATUS events.\n");
}

/* ------------------------------------------------------------------------ */
/* userspace interface (sf_ctl.c) */

static int sf_ctl_report_key_event(struct input_dev *input, sf_key_event_t *kevent)
{
	unsigned int key_code = KEY_UNKNOWN;

	switch (kevent->key) {
	case SF_KEY_HOME:
		key_code = KEY_HOME;
		break;
	case SF_KEY_MENU:
		key_code = KEY_MENU;
		break;
	case SF_KEY_BACK:
		key_code = KEY_BACK;
		break;
	case SF_KEY_F11:
		key_code = XIAOMI_KEYCODE_SINGLE_CLK;
		break;
	case SF_KEY_ENTER:
		key_code = KEY_ENTER;
		break;
	case SF_KEY_UP:
		key_code = KEY_UP;
		break;
	case SF_KEY_LEFT:
		key_code = KEY_LEFT;
		break;
	case SF_KEY_RIGHT:
		key_code = KEY_RIGHT;
		break;
	case SF_KEY_DOWN:
		key_code = KEY_DOWN;
		break;
	case SF_KEY_WAKEUP:
		key_code = KEY_WAKEUP;
		break;
	case SF_KEY_F12:
		key_code = XIAOMI_KEYCODE_DOUBLE_CLK;
		break;
	default:
		break;
	}

	input_report_key(input, key_code, kevent->value);
	input_sync(input);
	return 0;
}

static long sf_ctl_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	void __user *uarg = (void __user *)arg;
	sf_key_event_t kevent;
	int err = 0;

	switch (cmd) {
	case SF_IOC_INIT_DRIVER:
		mutex_lock(&sf_ctl_dev.lock);
		err = sf_ctl_device_init_gpio_pins();
		mutex_unlock(&sf_ctl_dev.lock);
		/* 4.14 ignored the result here */
		err = 0;
		break;

	case SF_IOC_DEINIT_DRIVER:
		mutex_lock(&sf_ctl_dev.lock);
		sf_ctl_device_free_gpio();
		mutex_unlock(&sf_ctl_dev.lock);
		break;

	case SPI_IOC_RST:
	case SF_IOC_RESET_DEVICE:
		sf_ctl_device_reset(false);
		msleep(1);
		sf_ctl_device_reset(true);
		msleep(10);
		break;

	case SF_IOC_REQUEST_IRQ:
		mutex_lock(&sf_ctl_dev.lock);
		sf_ctl_init_irq();
		mutex_unlock(&sf_ctl_dev.lock);
		break;

	case SF_IOC_ENABLE_POWER:
		mutex_lock(&sf_ctl_dev.lock);
		sf_ctl_device_power(true);
		mutex_unlock(&sf_ctl_dev.lock);
		break;

	case SF_IOC_REPORT_KEY_EVENT:
		if (copy_from_user(&kevent, uarg, sizeof(kevent))) {
			xprintk(KERN_ERR, "copy_from_user(..) failed.\n");
			err = -EFAULT;
			break;
		}
		err = sf_ctl_report_key_event(sf_ctl_dev.input, &kevent);
		break;

	/* SPI belongs to the TEE; no clock or speed control here */
	case SF_IOC_ENABLE_IRQ:
	case SF_IOC_DISABLE_IRQ:
	case SF_IOC_ENABLE_SPI_CLK:
	case SF_IOC_DISABLE_SPI_CLK:
	case SF_IOC_DISABLE_POWER:	/* kept on, as 4.14 */
	case SF_IOC_SYNC_CONFIG:
	case SPI_IOC_WR_MAX_SPEED_HZ:
	case SF_IOC_SPI_SPEED:
	case SPI_IOC_RD_MAX_SPEED_HZ:
		break;

	case FORTSENSE_IOC_ATTRIBUTE:
	case SF_IOC_ATTRIBUTE:
		err = put_user(sf_ctl_dev.attribute, (__u32 __user *)uarg);
		break;

	case SF_IOC_GET_VERSION: {
		char version[SF_DRV_VERSION_LEN] = SF_DRV_VERSION;

		if (copy_to_user(uarg, version, SF_DRV_VERSION_LEN)) {
			xprintk(KERN_ERR, "copy_to_user(..) failed.\n");
			err = -EFAULT;
		}
		break;
	}

	case SF_IOC_SET_LIB_VERSION:
		if (copy_from_user(&sf_hw_ver, uarg, sizeof(sf_hw_ver))) {
			xprintk(KERN_ERR, "sf_hw_info_t copy_from_user(..) failed.\n");
			err = -EFAULT;
		}
		break;

	case SF_IOC_GET_LIB_VERSION:
		if (copy_to_user(uarg, &sf_hw_ver, sizeof(sf_hw_ver))) {
			xprintk(KERN_ERR, "sf_hw_info_t copy_to_user(..) failed.\n");
			err = -EFAULT;
		}
		break;

	case SF_IOC_SET_SPI_BUF_SIZE:
		sf_ctl_dev.spi_buf_size = arg;
		break;

	case SF_IOC_SET_RESET_OUTPUT:
		sf_ctl_device_reset(!!arg);
		break;

	default:
		err = -EINVAL;
		break;
	}

	return err;
}

static const struct file_operations sf_ctl_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= sf_ctl_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static ssize_t version_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	return sysfs_emit(buf, "%.*s\n", SF_MAX_VER_INFO_LEN, sf_hw_ver.driver);
}
static DEVICE_ATTR_RO(version);

static ssize_t chip_info_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	return sysfs_emit(buf,
		"chip   : %.*s %.*s\nid     : 0x0 lib:%.*s\nvendor : fw:%.*s\nmore   : fingerprint\n",
		SF_MAX_VER_INFO_LEN, sf_hw_ver.fortsense_id,
		SF_MAX_VER_INFO_LEN * 2, sf_hw_ver.ca_version,
		SF_MAX_VER_INFO_LEN, sf_hw_ver.algorithm,
		SF_MAX_VER_INFO_LEN, sf_hw_ver.firmware);
}
static DEVICE_ATTR_RO(chip_info);

static ssize_t tee_version_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf,
		"solution:%.*s\nca      :%.*s\nta      :%.*s\nalg     :%.*s\nnav     :%.*s\ndriver  :%.*s\nfirmware:%.*s\nsensor  :%.*s\nvendor  :%.*s\n",
		SF_MAX_VER_INFO_LEN, sf_hw_ver.tee_solution,
		SF_MAX_VER_INFO_LEN * 2, sf_hw_ver.ca_version,
		SF_MAX_VER_INFO_LEN * 2, sf_hw_ver.ta_version,
		SF_MAX_VER_INFO_LEN, sf_hw_ver.algorithm,
		SF_MAX_VER_INFO_LEN, sf_hw_ver.algo_nav,
		SF_MAX_VER_INFO_LEN, sf_hw_ver.driver,
		SF_MAX_VER_INFO_LEN, sf_hw_ver.firmware,
		SF_MAX_VER_INFO_LEN, sf_hw_ver.fortsense_id,
		SF_MAX_VER_INFO_LEN, sf_hw_ver.vendor_id);
}
static DEVICE_ATTR_RO(tee_version);

/*
 * 4.14 passed the value to fb_blank(NULL, ...), which dereferences the NULL
 * fb_info. Keep the node for the HAL, but only log.
 */
static ssize_t set_fun_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	int blank;

	if (kstrtoint(buf, 10, &blank))
		return -EINVAL;
	xprintk(KERN_INFO, "set_fun blank = %d ignored.\n", blank);
	return count;
}
static DEVICE_ATTR_WO(set_fun);

static struct attribute *sf_sysfs_entries[] = {
	&dev_attr_set_fun.attr,
	&dev_attr_tee_version.attr,
	&dev_attr_chip_info.attr,
	&dev_attr_version.attr,
	NULL
};

static const struct attribute_group sf_attribute_group = {
	.attrs = sf_sysfs_entries,
};

static const struct attribute_group *sf_attribute_groups[] = {
	&sf_attribute_group,
	NULL
};

static int sf_ctl_init_input(void)
{
	struct input_dev *input;
	int err;

	input = input_allocate_device();
	if (!input)
		return -ENOMEM;

	input->name = "uinput-fortsense";
	input->id.vendor = 0x0666;
	input->id.product = 0x0888;
	__set_bit(EV_KEY, input->evbit);
	__set_bit(KEY_HOME, input->keybit);
	__set_bit(KEY_MENU, input->keybit);
	__set_bit(KEY_BACK, input->keybit);
	__set_bit(XIAOMI_KEYCODE_SINGLE_CLK, input->keybit);
	__set_bit(KEY_ENTER, input->keybit);
	__set_bit(KEY_UP, input->keybit);
	__set_bit(KEY_LEFT, input->keybit);
	__set_bit(KEY_RIGHT, input->keybit);
	__set_bit(KEY_DOWN, input->keybit);
	__set_bit(XIAOMI_KEYCODE_DOUBLE_CLK, input->keybit);

	err = input_register_device(input);
	if (err) {
		xprintk(KERN_ERR, "input_register_device(..) = %d.\n", err);
		input_free_device(input);
		return -ENODEV;
	}

	sf_ctl_dev.input = input;
	return 0;
}

static int sf_probe(struct platform_device *pdev)
{
	int err;

	sf_ctl_dev.pdev = pdev;
	sf_ctl_dev.np = of_find_compatible_node(NULL, NULL, COMPATIBLE_SW_FP);
	if (!sf_ctl_dev.np) {
		xprintk(KERN_INFO, "no '%s' node, not a Fortsense board.\n",
			COMPATIBLE_SW_FP);
		return -ENODEV;
	}
	/* regulator_get() looks the supply up in the node */
	device_set_node(&pdev->dev, of_fwnode_handle(sf_ctl_dev.np));

	mutex_init(&sf_ctl_dev.lock);
	INIT_WORK(&sf_ctl_dev.work_queue, sf_ctl_device_event);

	sf_ctl_dev.wakelock = wakeup_source_register(&pdev->dev, "sf_wakelock");
	if (!sf_ctl_dev.wakelock) {
		err = -ENOMEM;
		goto put_node;
	}

	err = sf_ctl_init_input();
	if (err)
		goto unregister_ws;

	sf_ctl_dev.miscdev.minor = MISC_DYNAMIC_MINOR;
	sf_ctl_dev.miscdev.name = "fortsense_fp";
	sf_ctl_dev.miscdev.fops = &sf_ctl_fops;
	sf_ctl_dev.miscdev.groups = sf_attribute_groups;
	err = misc_register(&sf_ctl_dev.miscdev);
	if (err) {
		xprintk(KERN_ERR, "misc_register(..) = %d.\n", err);
		goto unregister_input;
	}

	xprintk(KERN_INFO, "%s ok, driver version '%s'\n", __func__, SF_DRV_VERSION);
	return 0;

unregister_input:
	input_unregister_device(sf_ctl_dev.input);
	sf_ctl_dev.input = NULL;
unregister_ws:
	wakeup_source_unregister(sf_ctl_dev.wakelock);
	sf_ctl_dev.wakelock = NULL;
put_node:
	device_set_node(&pdev->dev, NULL);
	of_node_put(sf_ctl_dev.np);
	sf_ctl_dev.np = NULL;
	return err;
}

static int sf_remove(struct platform_device *pdev)
{
	if (sf_ctl_dev.panel_cookie)
		panel_event_notifier_unregister(sf_ctl_dev.panel_cookie);
	sf_ctl_dev.panel_cookie = NULL;

	mutex_lock(&sf_ctl_dev.lock);
	sf_ctl_device_free_gpio();
	mutex_unlock(&sf_ctl_dev.lock);
	cancel_work_sync(&sf_ctl_dev.work_queue);

	misc_deregister(&sf_ctl_dev.miscdev);
	input_unregister_device(sf_ctl_dev.input);
	sf_ctl_dev.input = NULL;
	wakeup_source_unregister(sf_ctl_dev.wakelock);
	sf_ctl_dev.wakelock = NULL;

	device_set_node(&pdev->dev, NULL);
	of_node_put(sf_ctl_dev.np);
	sf_ctl_dev.np = NULL;
	return 0;
}

static struct platform_driver sf_driver = {
	.driver = {
		.name = "fortsense-fp",
	},
	.probe	= sf_probe,
	.remove	= sf_remove,
};

static int __init sf_ctl_driver_init(void)
{
	int err;

	/* 4.14 SF_REG_DEVICE_BY_DRIVER: the DT node is not a platform device */
	sf_device = platform_device_register_simple("fortsense-fp", 0, NULL, 0);
	if (IS_ERR(sf_device)) {
		err = PTR_ERR(sf_device);
		sf_device = NULL;
		xprintk(KERN_ERR, "%s, Failed to register platform device.\n", __func__);
		return err;
	}

	err = platform_driver_register(&sf_driver);
	if (err) {
		xprintk(KERN_ERR, "%s, Failed to register platform driver.\n", __func__);
		platform_device_unregister(sf_device);
		sf_device = NULL;
	}
	return err;
}

static void __exit sf_ctl_driver_exit(void)
{
	platform_driver_unregister(&sf_driver);
	platform_device_unregister(sf_device);
}

module_init(sf_ctl_driver_init);
module_exit(sf_ctl_driver_exit);

MODULE_DESCRIPTION("The device control driver for Fortsense's fingerprint sensor.");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Fortsense");
