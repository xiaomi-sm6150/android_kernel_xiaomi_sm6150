// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/regulator/consumer.h>
#include <linux/soc/qcom/altmode-glink.h>
#include <linux/atomic.h>
#include <linux/workqueue.h>
#include "dpin_cci_util.h"
#include "usbmux_ps8822.h"

/* Instantiate all tracepoints defined in lontium-lt7911uxc-trace.h */
#define CREATE_TRACE_POINTS
#include "lontium-lt7911uxc-trace.h"

/* LT7911 Power Sequence Delays (milliseconds) */
#define LT7911_VDD_DELAY_MS             20
#define LT7911_RST_HIGH_DELAY_MS        10
#define LT7911_1V1_EN_DELAY_MS          10
#define LT7911_3V3_EN_DELAY_MS          40
#define LT7911_RST_LOW_DELAY_MS         20
#define LT7911_POWER_DOWN_DELAY_MS      20
#define LT7911_POWER_UP_DELAY_MS       500
/* GPIO Values */
#define GPIO_LOW                        0

#define DPIN_CONFIGURE_MASK (0x1f)
#define DPIN_MUXCTRL_USB3P1  0x01
#define USB_SID_DISPLAYPORT  0xff01

/*
 * Drain-loop tuning constants.
 *
 * LT7911_DRAIN_MAX_RETRIES - maximum number of re-reads after detecting a
 *   coalesced interrupt before giving up (guards against edge-trigger storms).
 * LT7911_DRAIN_SETTLE_MS   - settle delay between drain-loop iterations to
 *   allow the LT7911 to stabilise after rapid plug/unplug events.
 */
#define LT7911_DRAIN_MAX_RETRIES  5
#define LT7911_DRAIN_SETTLE_MS    100

#define LT7911_DP_STATE_MS        1000

/*
 * Bits in the LT7911 interrupt-type / ready-state value (reg 0x84).  The chip
 * reports which parts of the stream description it has just refreshed, so the
 * bits also tell us which register banks are meaningful on this read.
 */
#define LT7911_IRQ_VIDEO_READY    BIT(0)
#define LT7911_IRQ_AUDIO_READY    BIT(1)

struct lt7911uxc_data {
	struct device *dev;
	struct altmode_client *amclient;
	struct mutex device_lock;
	struct delayed_work info_work;
	struct work_struct dpalt_work;
	struct work_struct fw_upgrade_work;
	atomic_t fw_upgrade_in_progress;
	bool fw_upgrade_from_sysfs;
	atomic_t int_event_cnt;
	int lt7911_reset_gpio;
	int lt7911_1v1_en_gpio;
	int lt7911_3v3_en_gpio;
	int lt7911_gpio0_gpio_irq;
	int lt7911_gpio0_irq;                 /* Linux IRQ number for gpio0 */
	struct regulator *lt7911_vdd;         /* LT7911 VDD supply (L4B) */
	bool connected;
	bool lt7911_poweron;
	int last_width;
	int last_height;
	int last_fps;
	int last_format;
	int last_afreq;
	int last_ach;
	bool have_video_info;
	bool have_audio_info;
	bool usb_mux_only;        /* USB-only connect: just switch PS8822, no LT7911 ops */
	int lanes;
	int orientation;
	u8 pan_ack_port_index;    /* port_index to use when sending PAN ACKs from dpalt_work */
	struct cci_util_handle *cci_handle;   /* handle to dpin_cci_util device, set at probe */
	struct usbmux_handle *usbmux_handle; /* handle to usbmux_ps8822 device, set at probe */
};

enum dpin_pin_assignment {
	DPAM_HPD_OUT,
	DPAM_HPD_A,
	DPAM_HPD_B,
	DPAM_HPD_C,
	DPAM_HPD_D,
	DPAM_HPD_E,
	DPAM_HPD_F,
};

enum dpin_send_msg_type {
	DPIN_PAN_EN = 0x10,
	DPIN_PAN_ACK,
	DPIN_READ_SEL,
	DPIN_SEND_ATTENTION,
};

/*
 * LT7911 firmware upgrade — sysfs nodes
 */

/* Name of the firmware binary requested via the kernel firmware loader */
#define LT7911_FW_NAME  "lt7911_fw.bin"

/*
 * State flags shared between the sysfs nodes and the boot-time auto-upgrade
 * path.  Protected by the driver-wide device_lock where concurrent access
 * is possible.
 */
static int  lt7911_firmware_debug_flag;
static bool lt7911_sysfs_registered;

static int lt7911_mipi_enable(struct lt7911uxc_data *lt7911, int enable);

static int lt7911_power_up(struct lt7911uxc_data *lt7911)
{
	int ret = 0;

	if (!lt7911)
		return -EINVAL;

	lockdep_assert_held(&lt7911->device_lock);

	if (lt7911->lt7911_poweron) {
		dev_dbg(lt7911->dev, "LT7911 already powered on\n");
		return 0;
	}

	dev_dbg(lt7911->dev, "LT7911 power up start\n");

	/* Step 1: Enable VDD regulator (L4B) */
	if (lt7911->lt7911_vdd) {
		ret = regulator_enable(lt7911->lt7911_vdd);
		if (ret) {
			dev_err(lt7911->dev, "Failed to enable lti7911 VDD: %d\n", ret);
			return ret;
		}
		msleep(LT7911_VDD_DELAY_MS);
	}

	/* Step 2: Assert RESET (active high = hold in reset) */
	if (gpio_is_valid(lt7911->lt7911_reset_gpio)) {
		dev_err(lt7911->dev, "lt7911_reset_gpio:%d setting to LOW\n",
			lt7911->lt7911_reset_gpio);
		gpio_set_value(lt7911->lt7911_reset_gpio, 0);
		msleep(LT7911_RST_HIGH_DELAY_MS);
	}

	/* Step 3: Enable 1.1V rail */
	if (gpio_is_valid(lt7911->lt7911_1v1_en_gpio)) {
		dev_err(lt7911->dev, "Setting lt7911->lt7911_1v1_en_gpio:%d to High\n",
			   lt7911->lt7911_1v1_en_gpio);
		gpio_set_value(lt7911->lt7911_1v1_en_gpio, 1);
		msleep(LT7911_1V1_EN_DELAY_MS);
	}

	/* Step 4: Enable 3.3V rail */
	if (gpio_is_valid(lt7911->lt7911_3v3_en_gpio)) {
		dev_err(lt7911->dev, "Setting lt7911->lt7911_3v3_en_gpio:%d to High\n",
			   lt7911->lt7911_3v3_en_gpio);
		gpio_set_value(lt7911->lt7911_3v3_en_gpio, 1);
		msleep(LT7911_3V3_EN_DELAY_MS);
	}

	/* Step 5: Deassert RESET (low = release reset, chip starts) */
	if (gpio_is_valid(lt7911->lt7911_reset_gpio)) {
		dev_err(lt7911->dev, "lt7911_reset_gpio:%d setting to High\n",
			lt7911->lt7911_reset_gpio);
		gpio_set_value(lt7911->lt7911_reset_gpio, 1);
		msleep(LT7911_RST_LOW_DELAY_MS);
	}

	lt7911->lt7911_poweron = true;
	dev_dbg(lt7911->dev, "LT7911 power up complete\n");

	return ret;
}

static int lt7911_power_down(struct lt7911uxc_data *data)
{
	int ret = 0;

	if (!data)
		return -EINVAL;

	lockdep_assert_held(&data->device_lock);

	/* Release CCI power domain and clocks so SoC / AOSD can enter sleep */
	if (data->cci_handle)
		cci_util_lt7911_release_cci(data->cci_handle);

	if (!data->lt7911_poweron) {
		dev_dbg(data->dev, "LT7911 already powered off\n");
		return 0;
	}

	dev_dbg(data->dev, "LT7911 power down start\n");

	/* Step 1: Disable VDD regulator */
	if (data->lt7911_vdd) {
		ret = regulator_disable(data->lt7911_vdd);
		if (ret)
			dev_err(data->dev, "Failed to disable VDD regulator: %d\n", ret);
	}

	/* Step 2: Disable 3.3V rail */
	if (gpio_is_valid(data->lt7911_3v3_en_gpio)) {
		gpio_set_value_cansleep(data->lt7911_3v3_en_gpio, GPIO_LOW);
		msleep(LT7911_POWER_DOWN_DELAY_MS);
	}

	/* Step 3: Disable 1.1V rail */
	if (gpio_is_valid(data->lt7911_1v1_en_gpio))
		gpio_set_value_cansleep(data->lt7911_1v1_en_gpio, GPIO_LOW);

	data->lt7911_poweron = false;
	dev_dbg(data->dev, "LT7911 power down complete\n");

	return 0;
}

static void lt7911_notify_event(struct lt7911uxc_data *lt7911, int irq, int w, int h, int fps,
					int format, int afreq, int ach)
{
	char action[32], state[32], width[32], height[32], media_fps[32];
	char media_format[32], media_afreq[32], media_ach[32];
	char *envp[9];

	if (!lt7911 || !lt7911->dev)
		return;

	snprintf(action, sizeof(action), "ACTION=DPIN_HOST_INFO");
	if (irq == 0)
		snprintf(state, sizeof(state), "STATE=VIDEO_OR_AUDIO_NOT_READY");
	else if (irq == 1)
		snprintf(state, sizeof(state), "STATE=VIDEO_READY");
	else if (irq == 2)
		snprintf(state, sizeof(state), "STATE=AUDIO_READY");
	else if (irq == 3)
		snprintf(state, sizeof(state), "STATE=VIDEO_AUDIO_READY");
	else if (irq > 3)
		snprintf(state, sizeof(state), "STATE=HDR_STR_READY");
	else
		snprintf(state, sizeof(state), "STATE=UNKNOWN");
	snprintf(width, sizeof(width), "WIDTH=%d", w);
	snprintf(height, sizeof(height), "HEIGHT=%d", h);
	snprintf(media_fps, sizeof(media_fps), "FPS=%d.%02d", fps / 100, fps % 100);
	if (format == 0x00)
		snprintf(media_format, sizeof(media_format), "FORMAT=YUV422_8bit");
	else if (format == 0x01)
		snprintf(media_format, sizeof(media_format), "FORMAT=YUV422_10bit");
	else if (format == 0x02)
		snprintf(media_format, sizeof(media_format), "FORMAT=RGB888_8bit");
	else if (format == 0x03)
		snprintf(media_format, sizeof(media_format), "FORMAT=YUV420");
	else
		snprintf(media_format, sizeof(media_format), "FORMAT=UNKNOWN");
	snprintf(media_afreq, sizeof(media_afreq), "AUDIO_FREQ=%dKhz", afreq);
	snprintf(media_ach, sizeof(media_ach), "AUDIO_CHANNEL=%d", ach);

	envp[0] = action;
	envp[1] = state;
	envp[2] = width;
	envp[3] = height;
	envp[4] = media_fps;
	envp[5] = media_format;
	envp[6] = media_afreq;
	envp[7] = media_ach;
	envp[8] = NULL;

	dev_dbg(lt7911->dev, "irq:%d, w:%d, h:%d, fps:%d.%02d, format:%d, afreq:%d, ach:%d\n",
			irq, w, h, fps / 100, fps % 100, format, afreq, ach);
	trace_lt7911_uevent_sent(irq, w, h, fps, format, afreq, ach);
	kobject_uevent_env(&lt7911->dev->kobj, KOBJ_CHANGE, envp);
}

static void lt7911uxc_send_pan_ack(struct lt7911uxc_data *lt7911,
				u8 msg_type, u8 port_index)
{
	int rc;
	struct altmode_pan_ack_msg ack;

	ack.cmd_type = msg_type;
	ack.port_index = port_index;

	rc = altmode_send_data(lt7911->amclient, &ack, sizeof(ack));
	if (rc < 0) {
		dev_err(lt7911->dev, "failed to send data, rc:%d\n", rc);
		return;
	}

	dev_dbg(lt7911->dev, "msg_type:%d port=%d\n", msg_type, port_index);
}

/**
 * lt7911uxc_dpalt_work_fn - deferred worker for DP-Alt connect/disconnect.
 * @work: embedded work_struct from struct lt7911uxc_data
 *
 * Performs the slow, sleepable power-sequencing and usbmux configuration
 * triggered by a DP-Alt connect or disconnect event.
 */
static void lt7911uxc_dpalt_work_fn(struct work_struct *work)
{
	struct lt7911uxc_data *lt7911 =
		container_of(work, struct lt7911uxc_data, dpalt_work);
	int rc;
	bool connected;
	bool usb_mux;
	int lanes, orientation;
	u8 port_index;

	mutex_lock(&lt7911->device_lock);
	connected   = lt7911->connected;
	lanes       = lt7911->lanes;
	orientation = lt7911->orientation;
	port_index  = lt7911->pan_ack_port_index;
	usb_mux     = lt7911->usb_mux_only;
	lt7911->usb_mux_only = false;
	mutex_unlock(&lt7911->device_lock);

	/*
	 * A true usb_mux means a non-DP source is connected.
	 * In this case, we only set the orientation and return,
	 * skipping the remaining DP setup steps.
	 */
	if (usb_mux && !connected) {
		dev_dbg(lt7911->dev, "dpalt_work: PS8822 switch orientation=%d\n",
			    orientation);
		usbmux_setmode(lt7911->usbmux_handle, 0, orientation);
		return;
	}

	if (!connected) {
		/* Cable detach */
		dev_dbg(lt7911->dev, "dpalt_work: cable detached, powering down\n");
		usbmux_setmode(lt7911->usbmux_handle, lanes, orientation);
		usbmux_sethpd(lt7911->usbmux_handle, false);

		mutex_lock(&lt7911->device_lock);
		lt7911_power_down(lt7911);
		lt7911->have_video_info = false;
		lt7911->have_audio_info = false;
		mutex_unlock(&lt7911->device_lock);

		/*
		 * Notify userspace that the stream is gone: all fields zeroed
		 * signals VIDEO_OR_AUDIO_NOT_READY / disconnected.
		 */
		lt7911_notify_event(lt7911, 0, 0, 0, 0, 0, 0, 0);
		lt7911uxc_send_pan_ack(lt7911, DPIN_PAN_ACK, port_index);
	} else {
		/* Cable attach */
		dev_dbg(lt7911->dev, "dpalt_work: cable attached, lanes=%d orientation=%d\n",
			    lanes, orientation);

		mutex_lock(&lt7911->device_lock);
		rc = lt7911_power_up(lt7911);
		if (rc) {
			dev_err(lt7911->dev,
				"dpalt_work: powering up the LT7911 failed rc=%d\n",
				rc);
			lt7911->connected = false;
			mutex_unlock(&lt7911->device_lock);
			return;
		}
		mutex_unlock(&lt7911->device_lock);

		dev_dbg(lt7911->dev, "dpalt_work: powered up LT7911, switching usbmux to %d+%d\n",
			    lanes, orientation);
		usbmux_setmode(lt7911->usbmux_handle, lanes, orientation);
		usbmux_sethpd(lt7911->usbmux_handle, true);
		lt7911uxc_send_pan_ack(lt7911, DPIN_PAN_ACK, port_index);
		dev_dbg(lt7911->dev, "sending the Attention Message ack to ADSP PD\n");
		trace_lt7911_attention_ack(port_index, DPIN_SEND_ATTENTION);
		lt7911uxc_send_pan_ack(lt7911, DPIN_SEND_ATTENTION, port_index);
	}
}

static void lt7911_info_work_fn(struct work_struct *work)
{
	struct lt7911uxc_data *lt7911 =
		container_of(to_delayed_work(work), struct lt7911uxc_data, info_work);
	int irq = 0, width = 0, height = 0, fps = 0, format = 0, afreq = 0, ach = 0;
	int snapshot, rc, retries = 0;
	bool video_live, audio_live, suppress;

	/*
	 * Drain-loop for hotplug robustness:
	 *
	 * During hotplug, multiple GPIO0 IRQs can fire while
	 * we are inside the I2C read path (which can take ~100 ms due to the CCI
	 * power cycle).
	 *
	 * Here we snapshot the counter before the I2C transaction
	 * and compare it afterwards.  If it changed, at least one IRQ was
	 * coalesced, so we loop and re-read to capture the latest state.  A short
	 * settle delay gives the LT7911 time to stabilise after rapid plug/unplug.
	 */
	do {
		snapshot = atomic_read(&lt7911->int_event_cnt);
		irq = 0;
		width = 0;
		height = 0;
		fps = 0;
		format = 0;
		afreq = 0;
		ach = 0;

		mutex_lock(&lt7911->device_lock);
		if (!lt7911->lt7911_poweron) {
			mutex_unlock(&lt7911->device_lock);
			dev_dbg(lt7911->dev, "device powered off, set DP state\n");
			if (usbmux_setdpstate(lt7911->usbmux_handle, true))
				dev_warn(lt7911->dev,
					"usbmux_setdpstate(true) failed, dp_state not set\n");
			return;
		}
		mutex_unlock(&lt7911->device_lock);

		if (!lt7911->cci_handle) {
			dev_err(lt7911->dev,
				"cci_handle not available, skipping info read\n");
			return;
		}

		rc = cci_util_lt7911_read_chip_id(lt7911->cci_handle);
		if (rc)
			dev_err(lt7911->dev, "Failed to read chip id from LT7911: %d\n", rc);

		cci_util_lt7911_enable_i2c(lt7911->cci_handle);
		cci_util_lt7911_get_interrupt_type(lt7911->cci_handle, &irq);
		if (irq) {
			rc = cci_util_lt7911_get_information(lt7911->cci_handle,
							     &irq, &width, &height, &fps,
							     &format, &afreq, &ach);
			if (rc)
				dev_err(lt7911->dev,
					"Failed to get information from LT7911: %d\n",
					rc);
		}
		cci_util_lt7911_disable_i2c(lt7911->cci_handle);

		/*
		 * The LT7911 only refreshes the register bank belonging to the
		 * event that raised GPIO0.
		 */
		video_live = (width > 0 && height > 0);
		audio_live = (afreq > 0);

		mutex_lock(&lt7911->device_lock);

		if (irq & LT7911_IRQ_VIDEO_READY) {
			if (video_live) {
				lt7911->last_width = width;
				lt7911->last_height = height;
				lt7911->last_fps = fps;
				lt7911->last_format = format;
				lt7911->have_video_info = true;
			} else if (lt7911->have_video_info) {
				width = lt7911->last_width;
				height = lt7911->last_height;
				fps = lt7911->last_fps;
				format = lt7911->last_format;
				dev_dbg(lt7911->dev,
					"video registers stale, using cached %dx%d\n",
					width, height);
			}
		}

		if (irq & LT7911_IRQ_AUDIO_READY) {
			if (audio_live) {
				lt7911->last_afreq = afreq;
				lt7911->last_ach = ach;
				lt7911->have_audio_info = true;
			} else if (lt7911->have_audio_info) {
				afreq = lt7911->last_afreq;
				ach = lt7911->last_ach;
				dev_dbg(lt7911->dev,
					"audio registers stale, using cached %dKhz/%dch\n",
					afreq, ach);
			}
		}

		/*
		 * Only a connected cable with nothing at all reported is treated
		 * as a spurious read worth dropping.
		 */
		suppress = lt7911->connected && !irq;

		mutex_unlock(&lt7911->device_lock);

		if (suppress) {
			dev_dbg(lt7911->dev,
				"Ignore notification when connected and registers indicate 0\n");
		} else {
			lt7911_mipi_enable(lt7911, 1);
			lt7911_notify_event(lt7911, irq, width, height, fps, format, afreq, ach);
		}

		/* No new IRQs arrived while we were reading — we are done. */
		if (atomic_read(&lt7911->int_event_cnt) == snapshot)
			break;
		if (++retries >= LT7911_DRAIN_MAX_RETRIES) {
			dev_warn(lt7911->dev, "drain-loop hit max retries (%d), hotplug storm?\n",
				    retries);
			break;
		}
		msleep(LT7911_DRAIN_SETTLE_MS);
	} while (true);
}

static int lt7911uxc_dpalt_notify(void *priv, void *payload_data, size_t len)
{
	int rc = 0;
	struct lt7911uxc_data *lt7911 = (struct lt7911uxc_data *) priv;
	u8 port_index, dp_data, pin;
	u8 *payload = (u8 *) payload_data;
	int  local_lanes;
	bool newly_connected = false;

	if (len < 9) {
		dev_err(lt7911->dev, "payload too short: %zu\n", len);
		return -EINVAL;
	}

	port_index = payload[0];
	dp_data = payload[8];
	pin = dp_data & DPIN_CONFIGURE_MASK;

	dev_dbg(lt7911->dev, "payload=0x%x\n", dp_data);
	dev_dbg(lt7911->dev, "port_index=%d, pin=%d\n", port_index, pin);

	mutex_lock(&lt7911->device_lock);

	if (payload[1] == 0 || payload[1] == 1)
		lt7911->orientation = payload[1];
	else
		dev_warn(lt7911->dev, "Invalid orientation %d, must be 0 or 1\n", payload[1]);

	dev_dbg(lt7911->dev, "orientation:%d connected=%d\n",
		lt7911->orientation, lt7911->connected);

	if (!pin) {
		/* Cable detach */
		if (lt7911->connected) {
			lt7911->lanes = 0;
			lt7911->connected = false;
			dev_dbg(lt7911->dev, "DPIN cable is removed...\n");

			lt7911->pan_ack_port_index = port_index;
			mutex_unlock(&lt7911->device_lock);
			/*
			 * Using the freezable workqueue guarantees that background tasks are
			 * automatically frozen during suspend and only thawed after all drivers
			 * have completed their system resume callbacks (restoring runtime PM),
			 * preventing race-prone I2C transactions.
			 */
			queue_work(system_freezable_wq, &lt7911->dpalt_work);
			return rc;
		}

		/*
		 * If we receive 'not configured' and 'not dp connected',
		 * we must process the orientation immediately rather than waiting for
		 * DP alt mode completion.
		 *
		 * This ensures non-DP sources get the correct orientation.
		 *
		 * When payload[2] is DPIN_MUXCTRL_USB3P1, usb_mux_only is set to true,
		 * the correct port is populated, and dpalt_work is scheduled for
		 * subsequent processing.
		 *
		 * CC orientation PAN for PS8822 mux switching (mux_ctrl == USB3P1)
		 */
		if (payload[2] == DPIN_MUXCTRL_USB3P1) {
			lt7911->usb_mux_only = true;
			lt7911->pan_ack_port_index = port_index;
			mutex_unlock(&lt7911->device_lock);
			queue_work(system_freezable_wq, &lt7911->dpalt_work);
			return rc;
		}
		mutex_unlock(&lt7911->device_lock);
		return rc;
	}

	if (!lt7911->connected) {
		lt7911->usb_mux_only = false;
		lt7911->connected = true;
		dev_dbg(lt7911->dev, "DPIN cable is connected...\n");
		if ((pin == DPAM_HPD_B) || (pin == DPAM_HPD_D) || (pin == DPAM_HPD_F)) {
			lt7911->lanes = 2;
		} else if ((pin == DPAM_HPD_A) || (pin == DPAM_HPD_C) || (pin == DPAM_HPD_E)) {
			lt7911->lanes = 4;
		} else {
			lt7911->lanes = 0;
			lt7911->connected = false;
		}

		dev_dbg(lt7911->dev, "number of lanes:%d\n", lt7911->lanes);
		trace_lt7911_dpin_connected(port_index, lt7911->lanes,
					    lt7911->orientation);

		newly_connected = lt7911->connected;
	}

	local_lanes = lt7911->lanes;
	lt7911->pan_ack_port_index = port_index;

	mutex_unlock(&lt7911->device_lock);

	if (newly_connected && local_lanes > 0) {
		lt7911_notify_event(lt7911, -1, 0, 0, 0, 0, 0, 0);
		queue_work(system_freezable_wq, &lt7911->dpalt_work);
	}

	return rc;
}

static int lt7911uxc_dpalt_register(struct lt7911uxc_data *lt7911)
{
	struct altmode_client_data cd = {
		.callback = &lt7911uxc_dpalt_notify,
	};

	cd.name = "dp_in";
	cd.priv = lt7911;
	cd.svid = USB_SID_DISPLAYPORT;

	lt7911->amclient = altmode_register_client(lt7911->dev, &cd);
	if (IS_ERR_OR_NULL(lt7911->amclient)) {
		int err;

		err = IS_ERR(lt7911->amclient) ? PTR_ERR(lt7911->amclient) : -ENODEV;
		dev_dbg(lt7911->dev, "failed to register as altmode client: %d\n", err);
		return err;
	}

	dev_dbg(lt7911->dev, "Success in registering as client to altmode\n");
	return 0;
}

static int lt7911uxc_parse_dts(struct lt7911uxc_data *lt7911)
{
	lt7911->lt7911_reset_gpio =
		of_get_named_gpio(lt7911->dev->of_node, "reset-gpio", 0);
	if (!gpio_is_valid(lt7911->lt7911_reset_gpio)) {
		dev_err(lt7911->dev, "lt7911 can't find 'reset-gpio' in DT block\n");
		lt7911->lt7911_reset_gpio = -EINVAL;
	} else {
		if (devm_gpio_request_one(lt7911->dev, lt7911->lt7911_reset_gpio,
					GPIOF_OUT_INIT_LOW, "lt7911-reset")) {
			dev_err(lt7911->dev, "Failed to request lt7911 Reset gpio\n");
			return -ENODEV;
		}
	}

	lt7911->lt7911_1v1_en_gpio =
		of_get_named_gpio(lt7911->dev->of_node, "1v1-en-gpio", 0);
	if (!gpio_is_valid(lt7911->lt7911_1v1_en_gpio)) {
		dev_err(lt7911->dev, "lt7911 can't find '1v1_en_gpio' in DT block\n");
		lt7911->lt7911_1v1_en_gpio = -EINVAL;
	} else {
		if (devm_gpio_request_one(lt7911->dev, lt7911->lt7911_1v1_en_gpio,
					GPIOF_OUT_INIT_LOW, "lt7911-1v1-en")) {
			dev_err(lt7911->dev, "Failed to request lt7911 1v1-en gpio\n");
			return -ENODEV;
		}
	}

	lt7911->lt7911_3v3_en_gpio =
		of_get_named_gpio(lt7911->dev->of_node, "3v3-en-gpio", 0);
	if (!gpio_is_valid(lt7911->lt7911_3v3_en_gpio)) {
		dev_err(lt7911->dev, "lt7911 can't find '3v3_en_gpio' in DT block\n");
		lt7911->lt7911_3v3_en_gpio = -EINVAL;
	} else {
		if (devm_gpio_request_one(lt7911->dev, lt7911->lt7911_3v3_en_gpio,
					GPIOF_OUT_INIT_LOW, "lt7911-3v3-en")) {
			dev_err(lt7911->dev, "Failed to request lt7911 3v3-en gpio\n");
			return -ENODEV;
		}
	}

	lt7911->lt7911_gpio0_gpio_irq =
			of_get_named_gpio(lt7911->dev->of_node, "gpio0-irq-gpio", 0);
	if (!gpio_is_valid(lt7911->lt7911_gpio0_gpio_irq)) {
		dev_err(lt7911->dev, "lt7911 can't find 'gpio0_gpio_irq' in DT block\n");
		lt7911->lt7911_gpio0_gpio_irq = -EINVAL;
	} else {
		if (devm_gpio_request_one(lt7911->dev, lt7911->lt7911_gpio0_gpio_irq,
					GPIOF_IN, "lt7911-gpio0-irq")) {
			dev_err(lt7911->dev, "Failed to request lt7911 gpio0 IRQ gpio\n");
			return -ENODEV;
		}
	}

	lt7911->lt7911_vdd = devm_regulator_get_optional(lt7911->dev, "lt7911-vdd");
	if (IS_ERR(lt7911->lt7911_vdd)) {
		dev_dbg(lt7911->dev, "LT7911 VDD regulator not defined: %ld\n",
				PTR_ERR(lt7911->lt7911_vdd));
		lt7911->lt7911_vdd = NULL;
	}

	lt7911->lt7911_poweron = false;
	dev_dbg(lt7911->dev,
		"LT7911 GPIO/regulator parsing complete (rst=%s, 1v1=%s, 3v3=%s, vdd=%s)\n",
		gpio_is_valid(lt7911->lt7911_reset_gpio) ? "ok" : "n/a",
		gpio_is_valid(lt7911->lt7911_1v1_en_gpio) ? "ok" : "n/a",
		gpio_is_valid(lt7911->lt7911_3v3_en_gpio) ? "ok" : "n/a",
		lt7911->lt7911_vdd ? "ok" : "n/a");

	return 0;
}

/**
 * lt7911_gpio0_irq_handler - Hard-IRQ handler for the LT7911 GPIO0 interrupt.
 *
 * GPIO0 on the LT7911UXC is asserted by the chip whenever the input stream
 * state changes (video/audio ready, resolution change, hot-plug, etc.).
 * The handler increments int_event_cnt so the drain-loop in
 * lt7911_info_work_fn() can detect coalesced interrupts, cancels any
 * pending (not yet started) delayed work to restart the debounce window,
 * then reschedules the work item for the slow-path I2C reads.
 */

static irqreturn_t lt7911_gpio0_irq_handler(int irq, void *dev_id)
{
	struct lt7911uxc_data *lt7911 = dev_id;
	int event_cnt;

	dev_dbg(lt7911->dev, "GPIO0 IRQ fired (irq=%d)\n", irq);

	/*
	 * Suppress stream-state processing while a firmware upgrade is in
	 * progress.  The chip is being reflashed and its I2C registers are
	 * not in a valid state; scheduling info_work now would produce
	 * spurious reads.  lt7911uxc_firmware_cb() clears the flag and
	 * schedules info_work itself once the upgrade is complete.
	 */
	if (atomic_read(&lt7911->fw_upgrade_in_progress)) {
		dev_dbg(lt7911->dev, "GPIO0 IRQ suppressed: firmware upgrade in progress\n");
		return IRQ_HANDLED;
	}

	/*
	 * Atomically record that an interrupt has arrived.  The drain-loop in
	 * lt7911_info_work_fn() reads this counter before and after the I2C
	 * transaction; if it changed, at least one interrupt was coalesced and
	 * the loop re-reads the hardware state.
	 */
	event_cnt = atomic_inc_return(&lt7911->int_event_cnt);
	trace_lt7911_gpio0_irq(irq, event_cnt);
	cancel_delayed_work(&lt7911->info_work);
	queue_delayed_work(system_freezable_wq, &lt7911->info_work,
			msecs_to_jiffies(LT7911_DRAIN_SETTLE_MS));
	return IRQ_HANDLED;
}

static int lt7911uxc_register_gpio0_irq(struct lt7911uxc_data *lt7911)
{
	int irq, rc;

	if (!gpio_is_valid(lt7911->lt7911_gpio0_gpio_irq)) {
		dev_warn(lt7911->dev, "gpio0 IRQ GPIO not available, skipping IRQ registration\n");
		return 0;
	}

	irq = gpio_to_irq(lt7911->lt7911_gpio0_gpio_irq);
	if (irq < 0) {
		dev_err(lt7911->dev, "Failed to map gpio0 GPIO %d to IRQ: %d\n",
			   lt7911->lt7911_gpio0_gpio_irq, irq);
		return irq;
	}

	/*
	 * Trigger on the falling edge: the LT7911 pulses GPIO0 high when a
	 * stream-state change event is ready to be read back over I2C.
	 */
	rc = devm_request_threaded_irq(lt7911->dev, irq,
				       lt7911_gpio0_irq_handler,
				       NULL,
				       IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
				       "lt7911-gpio0-irq",
				       lt7911);
	if (rc) {
		dev_err(lt7911->dev, "Failed to request GPIO0 IRQ %d: %d\n", irq, rc);
		return rc;
	}

	lt7911->lt7911_gpio0_irq = irq;
	dev_dbg(lt7911->dev, "Registered GPIO0 IRQ %d (gpio=%d)\n",
		    irq, lt7911->lt7911_gpio0_gpio_irq);
	return 0;
}

/**
 * lt7911uxc_firmware_cb - async firmware load callback.
 * @fw:      firmware blob supplied by the kernel firmware loader, or NULL
 *           if the load timed out
 * @context: pointer to the lt7911uxc_data instance
 *
 * Called by the kernel firmware loader once "lt7911_fw.bin" has been
 * located (or the load has timed out).  Performs the actual flash upgrade
 * via cci_util_lt7911_do_firmware_upgrade(), power-cycles the chip so the
 * new firmware takes effect, then schedules info_work to set the DP state.
 */
static void lt7911uxc_firmware_cb(const struct firmware *fw, void *context)
{
	struct lt7911uxc_data *lt7911 = context;
	bool from_sysfs;
	int rc;

	if (!fw) {
		dev_err(lt7911->dev, "firmware load failed (fw == NULL)\n");
		goto powerdown;
	}

	dev_dbg(lt7911->dev, "firmware loaded (%zu bytes), starting upgrade\n", fw->size);

	if (!lt7911->cci_handle) {
		dev_err(lt7911->dev, "cci_handle not available, cannot upgrade firmware\n");
		goto release;
	}

	rc = cci_util_lt7911_do_firmware_upgrade(lt7911->cci_handle, fw);
	if (rc < 0) {
		dev_err(lt7911->dev, "firmware upgrade failed rc=%d\n", rc);
		goto release;
	}

	dev_dbg(lt7911->dev, "firmware upgrade succeeded, power cycling LT7911\n");

release:
	release_firmware(fw);
powerdown:
	/* Ensure the chip is powered down on every error path */
	mutex_lock(&lt7911->device_lock);
	lt7911_power_down(lt7911);
	from_sysfs = lt7911->fw_upgrade_from_sysfs;
	lt7911->fw_upgrade_from_sysfs = false;
	mutex_unlock(&lt7911->device_lock);
	/*
	 * Clear the upgrade flag before (optionally) scheduling info_work so
	 * the GPIO0 IRQ handler resumes normal operation from this point on.
	 */
	atomic_set(&lt7911->fw_upgrade_in_progress, 0);
	if (!from_sysfs) {
		queue_delayed_work(system_freezable_wq, &lt7911->info_work,
				      msecs_to_jiffies(LT7911_DP_STATE_MS));
	} else {
		dev_dbg(lt7911->dev,
			"sysfs-triggered upgrade complete, skipping info_work scheduling\n");
	}
}

/**
 * firmware_upgrade_store - trigger a firmware upgrade.
 * @dev:   device the sysfs attribute belongs to
 * @attr:  device attribute descriptor
 * @buf:   user-space input buffer
 * @count: number of bytes in @buf
 *
 * Write "1" to skip the upgrade if the version already matches.
 * Write any other value (e.g. "0") to force an unconditional upgrade.
 *
 * Return: @count on success, negative errno on failure.
 */
static ssize_t firmware_upgrade_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct lt7911uxc_data *lt7911 = dev_get_drvdata(dev);
	int ret, update_data = 0;
	uint32_t fw_version;
	bool powered_up_here = false;

	if (!lt7911)
		return -ENODEV;

	mutex_lock(&lt7911->device_lock);
	if (lt7911->connected) {
		mutex_unlock(&lt7911->device_lock);
		dev_err(dev, "firmware upgrade not allowed while DP is connected\n");
		return -EBUSY;
	}
	mutex_unlock(&lt7911->device_lock);

	if (atomic_cmpxchg(&lt7911->fw_upgrade_in_progress, 0, 1) != 0) {
		dev_err(dev, "firmware upgrade already in progress\n");
		return -EBUSY;
	}

	ret = kstrtoint(buf, 10, &update_data);
	if (ret) {
		dev_err(dev, "kstrtoint error rc=%d\n", ret);
		goto err_clear_flag;
	}

	if (!lt7911->cci_handle) {
		dev_err(dev, "cci_handle not available\n");
		ret = -ENODEV;
		goto err_clear_flag;
	}

	mutex_lock(&lt7911->device_lock);
	if (!lt7911->lt7911_poweron) {
		ret = lt7911_power_up(lt7911);
		if (ret) {
			mutex_unlock(&lt7911->device_lock);
			dev_err(dev, "lt7911 power up failed rc=%d\n", ret);
			goto err_clear_flag;
		}
		powered_up_here = true;
		msleep(LT7911_POWER_UP_DELAY_MS);
	}
	mutex_unlock(&lt7911->device_lock);

	ret = cci_util_lt7911_read_chip_id(lt7911->cci_handle);
	if (ret)
		dev_err(dev, "Failed to read chip id from LT7911: %d\n", ret);

	if (update_data == 1) {
		/* Version-check: skip upgrade if firmware is already current */
		fw_version = cci_util_lt7911_get_version(lt7911->cci_handle);
		if (fw_version == LT7911UXC_VERSION_NUM) {
			dev_dbg(dev, "version 0x%x already current, skipping upgrade\n",
				    LT7911UXC_VERSION_NUM);
			ret = 0;
			goto err_powerdown;
		}
		dev_dbg(dev, "version mismatch (got 0x%x expected 0x%x), upgrading\n",
			    fw_version, LT7911UXC_VERSION_NUM);
	} else {
		dev_dbg(dev, "forced firmware upgrade requested\n");
	}

	mutex_lock(&lt7911->device_lock);
	lt7911->fw_upgrade_from_sysfs = true;
	mutex_unlock(&lt7911->device_lock);

	ret = request_firmware_nowait(THIS_MODULE, false, LT7911_FW_NAME,
				      lt7911->dev, GFP_KERNEL,
				      lt7911, lt7911uxc_firmware_cb);
	if (ret) {
		dev_err(dev, "request_firmware_nowait failed rc=%d\n", ret);
		mutex_lock(&lt7911->device_lock);
		lt7911->fw_upgrade_from_sysfs = false;
		mutex_unlock(&lt7911->device_lock);
		goto err_powerdown;
	}

	dev_dbg(dev, "firmware loader invoked\n");
	return count;

err_powerdown:
	if (powered_up_here) {
		mutex_lock(&lt7911->device_lock);
		lt7911_power_down(lt7911);
		mutex_unlock(&lt7911->device_lock);
	}
err_clear_flag:
	atomic_set(&lt7911->fw_upgrade_in_progress, 0);
	return ret ? ret : count;
}

/**
 * firmware_upgrade_show - display the current firmware version and HDCP key.
 * @dev:  device the sysfs attribute belongs to
 * @attr: device attribute descriptor
 * @buf:  output buffer (PAGE_SIZE bytes)
 *
 * Return: number of bytes written to @buf, or negative errno on failure.
 */
static ssize_t firmware_upgrade_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct lt7911uxc_data *lt7911 = dev_get_drvdata(dev);
	uint32_t fw_version;
	uint8_t hdcp_key[LT7911_HDCPKEY_SIZE] = { 0 };
	char key_content[LT7911_HDCPKEY_SIZE * 5 + 1] = { 0 };
	bool powered_up_here = false;
	int i, rc;

	if (!lt7911)
		return -ENODEV;

	if (!lt7911->cci_handle) {
		dev_err(dev, "cci_handle not available\n");
		return -ENODEV;
	}

	mutex_lock(&lt7911->device_lock);
	if (!lt7911->lt7911_poweron) {
		rc = lt7911_power_up(lt7911);
		if (rc) {
			mutex_unlock(&lt7911->device_lock);
			dev_err(dev, "lt7911 power up failed rc=%d\n", rc);
			return rc;
		}
		powered_up_here = true;
		msleep(LT7911_POWER_UP_DELAY_MS);
	}
	mutex_unlock(&lt7911->device_lock);

	rc = cci_util_lt7911_read_chip_id(lt7911->cci_handle);
	if (rc)
		dev_err(dev, "Failed to read chip id from LT7911: %d\n", rc);

	fw_version = cci_util_lt7911_get_version(lt7911->cci_handle);
	rc = cci_util_lt7911_read_hdcpkey(lt7911->cci_handle, hdcp_key, LT7911_HDCPKEY_SIZE);
	if (rc < 0)
		dev_err(dev, "HDCP key read failed rc=%d\n", rc);

	if (fw_version == LT7911UXC_VERSION_NUM)
		dev_dbg(dev, "firmware version correct: 0x%x\n",
			    LT7911UXC_VERSION_NUM);
	else
		dev_err(dev, "firmware version mismatch: got 0x%x expected 0x%x\n",
			   fw_version, LT7911UXC_VERSION_NUM);

	if (powered_up_here) {
		mutex_lock(&lt7911->device_lock);
		lt7911_power_down(lt7911);
		mutex_unlock(&lt7911->device_lock);
	}

	for (i = 0; i < LT7911_HDCPKEY_SIZE; i++)
		snprintf(key_content + i * 5, 6, "0x%02x ", hdcp_key[i]);

	return snprintf(buf, PAGE_SIZE,
			"lt7911_version is 0x%x  hdcpkey is %s\n",
			fw_version, key_content);
}

/**
 * firmware_debug_flag_store - enable or disable the firmware debug flag.
 * @dev:   device the sysfs attribute belongs to
 * @attr:  device attribute descriptor
 * @buf:   user-space input buffer
 * @count: number of bytes in @buf
 *
 * Setting the flag suppresses the boot-time auto-upgrade.
 * Write "1" to enable, "0" to disable.
 *
 * Return: @count on success, negative errno on failure.
 */
static ssize_t firmware_debug_flag_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	int ret, debug_flag = 0;

	ret = kstrtoint(buf, 10, &debug_flag);
	if (ret) {
		dev_err(dev, "kstrtoint error rc=%d\n", ret);
		return ret;
	}
	lt7911_firmware_debug_flag = (debug_flag == 1) ? 1 : 0;
	dev_dbg(dev, "lt7911_firmware_debug_flag set to %d\n",
		    lt7911_firmware_debug_flag);
	return count;
}

static ssize_t firmware_debug_flag_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", lt7911_firmware_debug_flag);
}

/**
 * lt7911_cc_switch_store - control the CC-lane switch register (0xb7).
 * @dev:   device the sysfs attribute belongs to
 * @attr:  device attribute descriptor
 * @buf:   user-space input buffer
 * @count: number of bytes in @buf
 *
 * Write "2" to select CC2 (reg_data = 1); any other value selects CC1 (0).
 *
 * Return: @count on success, negative errno on failure.
 */
static ssize_t lt7911_cc_switch_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct lt7911uxc_data *lt7911 = dev_get_drvdata(dev);
	int ret, switch_data = 0;
	struct dpin_cci_util_i2c_reg_array sw[] = {
		{ .reg_addr = 0xff, .reg_data = 0xe0, .delay = 0, .data_mask = 0 },
		{ .reg_addr = 0xb7, .reg_data = 0x00, .delay = 0, .data_mask = 0 },
		{ .reg_addr = 0xb8, .reg_data = 0x01, .delay = 0, .data_mask = 0 },
	};

	if (!lt7911)
		return -ENODEV;

	if (!lt7911->lt7911_poweron) {
		dev_err(dev, "lt7911 not powered on\n");
		return -ENODEV;
	}

	ret = kstrtoint(buf, 10, &switch_data);
	if (ret) {
		dev_err(dev, "kstrtoint error rc=%d\n", ret);
		return ret;
	}

	/* switch_data == 2 selects CC2 (reg_data = 1); default is CC1 (0) */
	if (switch_data == 2)
		sw[1].reg_data = 1;

	if (!lt7911->cci_handle) {
		dev_err(dev, "cci_handle not available\n");
		return -ENODEV;
	}

	ret = cci_util_lt7911_reg_write(lt7911->cci_handle, sw, ARRAY_SIZE(sw));
	if (ret < 0)
		dev_err(dev, "cc_switch write failed rc=%d\n", ret);

	return count;
}

static ssize_t lt7911_cc_switch_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct lt7911uxc_data *lt7911 = dev_get_drvdata(dev);
	int rc, val = 0;

	if (!lt7911)
		return -ENODEV;

	if (!lt7911->lt7911_poweron) {
		dev_err(dev, "lt7911 not powered on\n");
		return -ENODEV;
	}

	if (!lt7911->cci_handle) {
		dev_err(dev, "cci_handle not available\n");
		return -ENODEV;
	}

	rc = cci_util_lt7911_reg_read(lt7911->cci_handle, 0xb7, &val);
	if (rc < 0)
		dev_err(dev, "reg read 0xb7 failed rc=%d\n", rc);

	return snprintf(buf, PAGE_SIZE, "0x%x\n", val);
}

/**
 * lt7911_swap_apply_store - control the lane-swap apply register (0xb8).
 * @dev:   device the sysfs attribute belongs to
 * @attr:  device attribute descriptor
 * @buf:   user-space input buffer
 * @count: number of bytes in @buf
 *
 * Write "1" to apply the lane swap; any other value clears it.
 *
 * Return: @count on success, negative errno on failure.
 */
static ssize_t lt7911_swap_apply_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct lt7911uxc_data *lt7911 = dev_get_drvdata(dev);
	int ret, switch_data = 0;
	struct dpin_cci_util_i2c_reg_array sw[] = {
		{ .reg_addr = 0xff, .reg_data = 0xe0, .delay = 0, .data_mask = 0 },
		{ .reg_addr = 0xee, .reg_data = 0x01, .delay = 0, .data_mask = 0 },
		{ .reg_addr = 0xb8, .reg_data = 0x00, .delay = 0, .data_mask = 0 },
		{ .reg_addr = 0xff, .reg_data = 0xe0, .delay = 0, .data_mask = 0 },
		{ .reg_addr = 0xee, .reg_data = 0x00, .delay = 0, .data_mask = 0 },
	};

	if (!lt7911)
		return -ENODEV;

	if (!lt7911->lt7911_poweron) {
		dev_err(dev, "lt7911 not powered on\n");
		return -ENODEV;
	}

	ret = kstrtoint(buf, 10, &switch_data);
	if (ret) {
		dev_err(dev, "kstrtoint error rc=%d\n", ret);
		return ret;
	}

	if (switch_data == 1)
		sw[2].reg_data = 1;

	if (!lt7911->cci_handle) {
		dev_err(dev, "cci_handle not available\n");
		return -ENODEV;
	}

	ret = cci_util_lt7911_reg_write(lt7911->cci_handle, sw, ARRAY_SIZE(sw));
	if (ret < 0)
		dev_err(dev, "swap_apply write failed rc=%d\n", ret);

	return count;
}

static ssize_t lt7911_swap_apply_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct lt7911uxc_data *lt7911 = dev_get_drvdata(dev);
	int rc, val = 0;

	if (!lt7911)
		return -ENODEV;

	if (!lt7911->lt7911_poweron) {
		dev_err(dev, "lt7911 not powered on\n");
		return -ENODEV;
	}

	if (!lt7911->cci_handle) {
		dev_err(dev, "cci_handle not available\n");
		return -ENODEV;
	}

	rc = cci_util_lt7911_reg_read(lt7911->cci_handle, 0xb8, &val);
	if (rc < 0)
		dev_err(dev, "reg read 0xb8 failed rc=%d\n", rc);

	return snprintf(buf, PAGE_SIZE, "0x%x\n", val);
}

/**
 * lt7911_dp_traning_show - read the DP training status register (0xa4).
 * @dev:  device the sysfs attribute belongs to
 * @attr: device attribute descriptor
 * @buf:  output buffer (PAGE_SIZE bytes)
 *
 * Return: number of bytes written to @buf, or negative errno on failure.
 */
static ssize_t lt7911_dp_traning_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct lt7911uxc_data *lt7911 = dev_get_drvdata(dev);
	int rc, val = 0;

	if (!lt7911)
		return -ENODEV;

	if (!lt7911->lt7911_poweron) {
		dev_err(dev, "lt7911 not powered on\n");
		return -ENODEV;
	}

	if (!lt7911->cci_handle) {
		dev_err(dev, "cci_handle not available\n");
		return -ENODEV;
	}

	rc = cci_util_lt7911_reg_read(lt7911->cci_handle, 0xa4, &val);
	if (rc < 0)
		dev_err(dev, "dp training reg read failed rc=%d\n", rc);

	return snprintf(buf, PAGE_SIZE, "0x%x\n", val);
}

/**
 * lt7911_hdcp_version_show - read the HDCP version register (0x95).
 * @dev:  device the sysfs attribute belongs to
 * @attr: device attribute descriptor
 * @buf:  output buffer (PAGE_SIZE bytes)
 *
 * Return: number of bytes written to @buf, or negative errno on failure.
 */
static ssize_t lt7911_hdcp_version_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct lt7911uxc_data *lt7911 = dev_get_drvdata(dev);
	int rc, val = 0;

	if (!lt7911)
		return -ENODEV;

	if (!lt7911->lt7911_poweron) {
		dev_err(dev, "lt7911 not powered on\n");
		return -ENODEV;
	}

	if (!lt7911->cci_handle) {
		dev_err(dev, "cci_handle not available\n");
		return -ENODEV;
	}

	rc = cci_util_lt7911_reg_read(lt7911->cci_handle, 0x95, &val);
	if (rc < 0)
		dev_err(dev, "HDCP version reg read failed rc=%d\n", rc);

	return snprintf(buf, PAGE_SIZE, "%d\n", val);
}

/**
 * lt7911_stream_info_show - read and display the current stream information.
 * @dev:  device the sysfs attribute belongs to
 * @attr: device attribute descriptor
 * @buf:  output buffer (PAGE_SIZE bytes)
 *
 * Performs a live I2C read on every sysfs cat via
 * cci_util_lt7911_get_information(), bracketed by
 * cci_util_lt7911_enable_i2c() / cci_util_lt7911_disable_i2c(), and formats
 * all seven fields (irq/state, width, height, fps, format, audio
 * frequency, audio channel count) in the same vocabulary used by the uevent.
 *
 * Return: number of bytes written to @buf, or negative errno on failure.
 */
static ssize_t lt7911_stream_info_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct lt7911uxc_data *lt7911 = dev_get_drvdata(dev);
	int rc;
	int irq = 0, width = 0, height = 0, fps = 0, format = 0, afreq = 0, ach = 0;
	const char *state_str;
	const char *fmt_str;

	if (!lt7911)
		return -ENODEV;

	if (!lt7911->lt7911_poweron) {
		dev_err(dev, "lt7911 not powered on\n");
		return -ENODEV;
	}

	if (!lt7911->cci_handle) {
		dev_err(dev, "cci_handle not available\n");
		return -ENODEV;
	}

	rc = cci_util_lt7911_read_chip_id(lt7911->cci_handle);
	if (rc)
		dev_err(dev, "Failed to read chip id from LT7911: %d\n", rc);
	cci_util_lt7911_enable_i2c(lt7911->cci_handle);
	rc = cci_util_lt7911_get_information(lt7911->cci_handle,
					     &irq, &width, &height, &fps,
					     &format, &afreq, &ach);
	cci_util_lt7911_disable_i2c(lt7911->cci_handle);

	if (rc) {
		dev_err(dev, "cci_util_lt7911_get_information failed rc=%d\n", rc);
		return rc;
	}

	if (irq == 0)
		state_str = "VIDEO_OR_AUDIO_NOT_READY";
	else if (irq == 1)
		state_str = "VIDEO_READY";
	else if (irq == 2)
		state_str = "AUDIO_READY";
	else if (irq == 3)
		state_str = "VIDEO_AUDIO_READY";
	else if (irq > 3)
		state_str = "HDR_STR_READY";
	else
		state_str = "UNKNOWN";

	if (format == 0x00)
		fmt_str = "YUV422_8bit";
	else if (format == 0x01)
		fmt_str = "YUV422_10bit";
	else if (format == 0x02)
		fmt_str = "RGB888_8bit";
	else if (format == 0x03)
		fmt_str = "YUV420";
	else
		fmt_str = "UNKNOWN";

	return snprintf(buf, PAGE_SIZE,
			"STATE=%s\n"
			"WIDTH=%d\n"
			"HEIGHT=%d\n"
			"FPS=%d.%02d\n"
			"FORMAT=%s\n"
			"AUDIO_FREQ=%dKhz\n"
			"AUDIO_CHANNEL=%d\n",
			state_str,
			width,
			height,
			fps / 100, fps % 100,
			fmt_str,
			afreq,
			ach);
}

/**
 * lt7911_mipi_enable - enable or disable the LT7911 MIPI output (register 0xb0).
 * @enable: non-zero to enable MIPI output, zero to disable
 *
 * Writes the MIPI output enable/disable sequence to the LT7911 over I2C.
 * The caller must ensure the device is powered on before calling this
 * function.  It is safe to call from any sleepable context (e.g. a work
 * queue handler or a sysfs store callback).
 *
 * Return: 0 on success, negative errno on failure.
 */
static int lt7911_mipi_enable(struct lt7911uxc_data *lt7911, int enable)
{
	int rc;
	/*
	 * Index 3 (reg 0xb0) is patched at runtime to 0x01 (enable) or
	 * 0x00 (disable) before the write is issued.
	 */
	struct dpin_cci_util_i2c_reg_array mipi_ctrl[] = {
		{ .reg_addr = 0xff, .reg_data = 0xe0, .delay = 0, .data_mask = 0 },
		{ .reg_addr = 0xee, .reg_data = 0x01, .delay = 0, .data_mask = 0 },
		{ .reg_addr = 0xff, .reg_data = 0xe0, .delay = 0, .data_mask = 0 },
		{ .reg_addr = 0xb0, .reg_data = 0x00, .delay = 0, .data_mask = 0 },
		{ .reg_addr = 0xff, .reg_data = 0xe0, .delay = 0, .data_mask = 0 },
		{ .reg_addr = 0xee, .reg_data = 0x00, .delay = 0, .data_mask = 0 },
	};

	mipi_ctrl[3].reg_data = (u8)!!enable;

	if (!lt7911->cci_handle) {
		dev_err(lt7911->dev, "cci_handle not available\n");
		return -ENODEV;
	}

	rc = cci_util_lt7911_reg_write(lt7911->cci_handle,
				       mipi_ctrl, ARRAY_SIZE(mipi_ctrl));
	if (rc < 0)
		dev_err(lt7911->dev, "mipi %s write failed rc=%d\n",
			   enable ? "enable" : "disable", rc);
	return rc;
}

/**
 * lt7911_mipi_status_store - enable or disable the MIPI output (register 0xb0).
 * @dev:   device the sysfs attribute belongs to
 * @attr:  device attribute descriptor
 * @buf:   user-space input buffer
 * @count: number of bytes in @buf
 *
 * Write "1" to enable MIPI output, "0" to disable.
 *
 * Return: @count on success, negative errno on failure.
 */
static ssize_t lt7911_mipi_status_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct lt7911uxc_data *lt7911 = dev_get_drvdata(dev);
	int rc, mipi_en = 0;

	if (!lt7911)
		return -ENODEV;

	if (!lt7911->lt7911_poweron) {
		dev_err(dev, "lt7911 not powered on\n");
		return -ENODEV;
	}

	rc = kstrtoint(buf, 10, &mipi_en);
	if (rc) {
		dev_err(dev, "kstrtoint error rc=%d\n", rc);
		return rc;
	}

	dev_dbg(dev, "mipi status %d (1=enable 0=disable)\n", mipi_en);

	if (mipi_en == 1 || mipi_en == 0)
		lt7911_mipi_enable(lt7911, mipi_en);

	return count;
}

static ssize_t lt7911_mipi_status_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct lt7911uxc_data *lt7911 = dev_get_drvdata(dev);
	int rc, val = 0;

	if (!lt7911)
		return -ENODEV;

	if (!lt7911->lt7911_poweron) {
		dev_err(dev, "lt7911 not powered on\n");
		return -ENODEV;
	}

	if (!lt7911->cci_handle) {
		dev_err(dev, "cci_handle not available\n");
		return -ENODEV;
	}

	rc = cci_util_lt7911_reg_read(lt7911->cci_handle, 0xb0, &val);
	if (rc < 0)
		dev_err(dev, "mipi status reg read failed rc=%d\n", rc);

	return snprintf(buf, PAGE_SIZE, "%d\n", val);
}

/*
 * lt7911_reg_access — generic register read/write sysfs interface
 *
 * Allows userspace to execute arbitrary Lontium register sequences via a
 * simple 4-byte-per-command protocol.  Each command is four hex bytes
 * separated by spaces:
 *
 *   <slave> <reg> <data/count> <mask>
 *
 * Command encoding (slave byte is 0x86 for the LT7911):
 *   mask == 0x00  → WRITE reg_addr=<reg>, reg_data=<data>
 *   mask == 0xFF  → READ  <count> bytes starting at <reg>
 *   slave == 0x00 && reg == 0x00 && mask == 0x00
 *                 → DELAY of <data> * 100 ms
 *
 * Multiple commands are separated by whitespace (spaces or newlines).
 * Read results accumulate internally and are returned on the next cat of
 * the same node.
 *
 * Example — DP Lane Error Count sequence:
 *   echo "86 ff e0 00  86 ee 01 00  86 FF E1 00  86 00 02 FF \
 *         86 FF F1 00  86 A5 0F 00  86 A5 00 00  00 00 01 00 \
 *         86 CC 08 FF  86 ff e0 00  86 ee 00 00" \
 *     > /sys/devices/.../lt7911_reg_access
 *   cat /sys/devices/.../lt7911_reg_access
 *     # prints read results as hex bytes
 */

/* Maximum accumulated read data (bytes) */
#define LT7911_REG_ACCESS_BUF_SZ  256
/* Maximum commands per single write (4 bytes each) */
#define LT7911_REG_ACCESS_MAX_CMDS 64

/* Per-device storage for read results between store→show */
static u8  lt7911_reg_access_rdbuf[LT7911_REG_ACCESS_BUF_SZ];
static int lt7911_reg_access_rdlen;

/**
 * lt7911_reg_access_store - execute a register command sequence.
 * @dev:   device the sysfs attribute belongs to
 * @attr:  device attribute descriptor
 * @buf:   user-space input buffer (hex command string)
 * @count: number of bytes in @buf
 *
 * Parses and executes the command sequence described above.
 *
 * Return: @count on success, negative errno on failure.
 */
static ssize_t lt7911_reg_access_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct lt7911uxc_data *lt7911 = dev_get_drvdata(dev);
	const char *p = buf;
	unsigned int slave, reg, data, mask;
	int rc, n, rd_offset = 0;
	struct dpin_cci_util_i2c_reg_array wr;
	u8 rd_tmp[32];

	if (!lt7911)
		return -ENODEV;

	if (!lt7911->lt7911_poweron) {
		dev_err(dev, "lt7911 not powered on\n");
		return -ENODEV;
	}

	if (!lt7911->cci_handle) {
		dev_err(dev, "cci_handle not available\n");
		return -ENODEV;
	}

	/* Clear previous read results */
	lt7911_reg_access_rdlen = 0;
	memset(lt7911_reg_access_rdbuf, 0, sizeof(lt7911_reg_access_rdbuf));

	while (*p) {
		/* Skip whitespace and commas */
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',')
			p++;
		if (*p == '\0')
			break;

		/* Parse 4 hex values */
		n = sscanf(p, "%x %x %x %x", &slave, &reg, &data, &mask);
		if (n != 4) {
			dev_err(dev, "reg_access: parse error near '%.20s'\n", p);
			break;
		}

		/* Advance past the 4 tokens we just consumed */
		{
			int tokens = 0;

			while (tokens < 4 && *p) {
				/* skip whitespace before token */
				while (*p == ' ' || *p == '\t' || *p == '\n')
					p++;
				/* skip the token itself */
				while (*p && *p != ' ' && *p != '\t' && *p != '\n')
					p++;
				tokens++;
			}
		}

		dev_dbg(dev, "reg_access cmd: %02x %02x %02x %02x\n",
			slave, reg, data, mask);

		/* DELAY command: slave==0 && reg==0 && mask==0 */
		if (slave == 0x00 && reg == 0x00 && mask == 0x00) {
			unsigned int delay_ms = data * 100;

			dev_dbg(dev, "reg_access: delay %u ms\n", delay_ms);
			if (delay_ms > 5000)
				delay_ms = 5000; /* cap at 5s */
			msleep(delay_ms);
			continue;
		}

		/* READ command: mask == 0xFF */
		if (mask == 0xFF) {
			u16 num_bytes = (u16)data;

			if (num_bytes == 0 || num_bytes > 32) {
				dev_err(dev,
					"reg_access: invalid read count %u\n",
					num_bytes);
				continue;
			}
			if (rd_offset + num_bytes > LT7911_REG_ACCESS_BUF_SZ) {
				dev_err(dev,
					"reg_access: read buffer full\n");
				continue;
			}

			rc = cci_util_lt7911_reg_read_seq(lt7911->cci_handle,
							  (u8)reg, rd_tmp,
							  num_bytes);
			if (rc < 0) {
				dev_err(dev,
					"reg_access: read reg 0x%02x len %u failed rc=%d\n",
					reg, num_bytes, rc);
			} else {
				memcpy(&lt7911_reg_access_rdbuf[rd_offset],
				       rd_tmp, num_bytes);
				rd_offset += num_bytes;
			}
			continue;
		}

		/* WRITE command: mask == 0x00 (or anything else) */
		wr.reg_addr  = reg;
		wr.reg_data  = data;
		wr.delay     = 0;
		wr.data_mask = 0;

		rc = cci_util_lt7911_reg_write(lt7911->cci_handle, &wr, 1);
		if (rc < 0)
			dev_err(dev,
				"reg_access: write reg 0x%02x=0x%02x failed rc=%d\n",
				reg, data, rc);
	}

	lt7911_reg_access_rdlen = rd_offset;
	return count;
}

/**
 * lt7911_reg_access_show - return accumulated read data from the last command.
 * @dev:  device the sysfs attribute belongs to
 * @attr: device attribute descriptor
 * @buf:  output buffer (PAGE_SIZE bytes)
 *
 * Return: number of bytes written to @buf.
 */
static ssize_t lt7911_reg_access_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int i, len = 0;

	if (lt7911_reg_access_rdlen == 0)
		return snprintf(buf, PAGE_SIZE, "(no data)\n");

	for (i = 0; i < lt7911_reg_access_rdlen; i++) {
		len += snprintf(buf + len, PAGE_SIZE - len, "%02x ",
				lt7911_reg_access_rdbuf[i]);
		if (len >= PAGE_SIZE - 4)
			break;
	}
	len += snprintf(buf + len, PAGE_SIZE - len, "\n");
	return len;
}

static DEVICE_ATTR_RW(firmware_upgrade);
static DEVICE_ATTR_RW(firmware_debug_flag);
static DEVICE_ATTR_RW(lt7911_cc_switch);
static DEVICE_ATTR_RW(lt7911_swap_apply);
static DEVICE_ATTR_RO(lt7911_dp_traning);
static DEVICE_ATTR_RO(lt7911_hdcp_version);
static DEVICE_ATTR_RW(lt7911_mipi_status);
static DEVICE_ATTR_RO(lt7911_stream_info);
static DEVICE_ATTR_RW(lt7911_reg_access);

static struct attribute *lt7911_sysfs_attrs[] = {
	&dev_attr_firmware_upgrade.attr,
	&dev_attr_firmware_debug_flag.attr,
	&dev_attr_lt7911_cc_switch.attr,
	&dev_attr_lt7911_swap_apply.attr,
	&dev_attr_lt7911_dp_traning.attr,
	&dev_attr_lt7911_hdcp_version.attr,
	&dev_attr_lt7911_mipi_status.attr,
	&dev_attr_lt7911_stream_info.attr,
	&dev_attr_lt7911_reg_access.attr,
	NULL,
};

static struct attribute_group lt7911_attr_group = {
	.attrs = lt7911_sysfs_attrs,
};

/**
 * lt7911_create_sysfs - create the LT7911 sysfs attribute group.
 * @lt7911: driver instance whose kobject will host the attributes
 *
 * Called once from lt7911uxc_probe().  Idempotent: a second call is a no-op.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int lt7911_create_sysfs(struct lt7911uxc_data *lt7911)
{
	int rc;

	if (lt7911_sysfs_registered)
		return 0;

	rc = sysfs_create_group(&lt7911->dev->kobj, &lt7911_attr_group);
	if (rc) {
		dev_err(lt7911->dev, "sysfs_create_group failed rc=%d\n", rc);
		return rc;
	}
	lt7911_sysfs_registered = true;
	dev_dbg(lt7911->dev, "sysfs group created\n");
	return 0;
}

/**
 * lt7911_fw_upgrade_work_fn - worker that runs the post-probe firmware upgrade.
 * @work: embedded work_struct from struct lt7911uxc_data
 *
 * Scheduled once at the end of lt7911uxc_probe().  Powers up the LT7911,
 * reads the firmware version, and — if a mismatch is detected — hands off
 * to request_firmware_nowait() so the actual flash write happens
 * asynchronously in lt7911uxc_firmware_cb().  If no upgrade is needed,
 * schedules info_work directly so the driver becomes fully operational.
 *
 * The function is a no-op when lt7911_firmware_debug_flag is set.
 */
static void lt7911_fw_upgrade_work_fn(struct work_struct *work)
{
	struct lt7911uxc_data *lt7911 =
		container_of(work, struct lt7911uxc_data, fw_upgrade_work);
	uint32_t fw_version;
	int ret;

	atomic_set(&lt7911->fw_upgrade_in_progress, 1);

	if (lt7911_firmware_debug_flag) {
		dev_dbg(lt7911->dev, "debug flag set, skipping auto firmware upgrade\n");
		goto schedule;
	}

	/* Power up the chip so we can read the firmware version over I2C */
	mutex_lock(&lt7911->device_lock);
	ret = lt7911_power_up(lt7911);
	msleep(LT7911_POWER_UP_DELAY_MS);
	mutex_unlock(&lt7911->device_lock);
	if (ret) {
		dev_err(lt7911->dev, "fw_upgrade_work: power up failed rc=%d\n", ret);
		goto schedule;
	}

	if (!lt7911->cci_handle) {
		dev_err(lt7911->dev, "cci_handle not available, skipping firmware upgrade\n");
		goto powerdown;
	}

	ret = cci_util_lt7911_read_chip_id(lt7911->cci_handle);
	if (ret)
		dev_err(lt7911->dev, "Failed to read chip id from LT7911: %d\n", ret);

	fw_version = cci_util_lt7911_get_version(lt7911->cci_handle);
	if (fw_version == 0) {
		dev_dbg(lt7911->dev, "version read returned 0, skipping upgrade\n");
		goto powerdown;
	}
	if (fw_version == LT7911UXC_VERSION_NUM) {
		dev_dbg(lt7911->dev,
			    "firmware version 0x%x is current, no upgrade needed\n",
			    LT7911UXC_VERSION_NUM);
		goto powerdown;
	}

	dev_dbg(lt7911->dev,
		"version mismatch (got 0x%x expected 0x%x), requesting firmware\n",
		    fw_version, LT7911UXC_VERSION_NUM);
	/*
	 * Hand off to the kernel firmware loader.  lt7911uxc_firmware_cb()
	 * will flash the image, power-cycle the chip, clear the in-progress
	 * flag, and schedule info_work when it is done — no blocking here.
	 */
	ret = request_firmware_nowait(THIS_MODULE, false, LT7911_FW_NAME,
				      lt7911->dev, GFP_KERNEL,
				      lt7911, lt7911uxc_firmware_cb);
	if (ret) {
		dev_err(lt7911->dev, "request_firmware_nowait failed rc=%d\n", ret);
		goto powerdown;
	}

	/* lt7911uxc_firmware_cb() will power down, clear the flag, and
	 * schedule info_work on completion
	 */
	return;

powerdown:
	/* Power down the chip on any error path after a successful power-up */
	mutex_lock(&lt7911->device_lock);
	lt7911_power_down(lt7911);
	mutex_unlock(&lt7911->device_lock);
schedule:
	atomic_set(&lt7911->fw_upgrade_in_progress, 0);
	/*
	 * No upgrade was performed (version current, power-up failed, etc.).
	 * Schedule info_work so the DP state is set and the driver is ready.
	 */
	queue_delayed_work(system_freezable_wq, &lt7911->info_work,
					msecs_to_jiffies(LT7911_DP_STATE_MS));
}

static int lt7911uxc_probe(struct platform_device *pdev)
{
	struct lt7911uxc_data *lt7911 = NULL;
	int rc = 0;

	lt7911 = devm_kzalloc(&pdev->dev, sizeof(*lt7911), GFP_KERNEL);
	if (!lt7911)
		return -ENOMEM;

	mutex_init(&lt7911->device_lock);
	INIT_DELAYED_WORK(&lt7911->info_work, lt7911_info_work_fn);
	INIT_WORK(&lt7911->dpalt_work, lt7911uxc_dpalt_work_fn);
	INIT_WORK(&lt7911->fw_upgrade_work, lt7911_fw_upgrade_work_fn);
	atomic_set(&lt7911->fw_upgrade_in_progress, 0);
	atomic_set(&lt7911->int_event_cnt, 0);
	dev_set_drvdata(&pdev->dev, lt7911);
	lt7911->dev = &pdev->dev;

	rc = lt7911uxc_parse_dts(lt7911);
	if (rc < 0) {
		dev_err(lt7911->dev, "Failed to parse dts...\n");
		return rc;
	}

	rc = lt7911uxc_dpalt_register(lt7911);
	if (rc < 0) {
		dev_dbg(lt7911->dev, "Failed to register with dp altmode glink\n");
		return rc;
	}

	rc = lt7911uxc_register_gpio0_irq(lt7911);
	if (rc < 0) {
		dev_err(lt7911->dev, "Failed to register GPIO0 IRQ\n");
		altmode_deregister_client(lt7911->amclient);
		return rc;
	}

	rc = lt7911_create_sysfs(lt7911);
	if (rc < 0) {
		dev_err(lt7911->dev, "Failed to create sysfs group\n");
		if (lt7911->lt7911_gpio0_irq > 0)
			disable_irq(lt7911->lt7911_gpio0_irq);
		cancel_delayed_work_sync(&lt7911->info_work);
		altmode_deregister_client(lt7911->amclient);
		return rc;
	}

	/*
	 * Obtain the CCI-util handle.  This must succeed for any I2C
	 * operation to be possible.  If the dpin_cci_util driver has not
	 * yet probed, return -EPROBE_DEFER so the kernel retries later.
	 */
	lt7911->cci_handle = cci_util_lt7911_get_device();
	if (!lt7911->cci_handle) {
		dev_err(lt7911->dev, "CCI util not ready, deferring probe\n");
		if (lt7911_sysfs_registered) {
			sysfs_remove_group(&lt7911->dev->kobj, &lt7911_attr_group);
			lt7911_sysfs_registered = false;
		}
		if (lt7911->lt7911_gpio0_irq > 0)
			disable_irq(lt7911->lt7911_gpio0_irq);
		cancel_delayed_work_sync(&lt7911->info_work);
		altmode_deregister_client(lt7911->amclient);
		return -EPROBE_DEFER;
	}

	/*
	 * Obtain the usbmux handle.  If the usbmux_ps8822 driver has not yet
	 * probed, return -EPROBE_DEFER so the kernel retries later.
	 */
	lt7911->usbmux_handle = usbmux_get_device();
	if (!lt7911->usbmux_handle) {
		dev_err(lt7911->dev, "usbmux_ps8822 not ready, deferring probe\n");
		cci_util_lt7911_put_device(lt7911->cci_handle);
		lt7911->cci_handle = NULL;
		if (lt7911_sysfs_registered) {
			sysfs_remove_group(&lt7911->dev->kobj, &lt7911_attr_group);
			lt7911_sysfs_registered = false;
		}
		if (lt7911->lt7911_gpio0_irq > 0)
			disable_irq(lt7911->lt7911_gpio0_irq);
		cancel_delayed_work_sync(&lt7911->info_work);
		altmode_deregister_client(lt7911->amclient);
		return -EPROBE_DEFER;
	}

	dev_dbg(lt7911->dev, "Successfully probed..\n");
	/*
	 * Schedule the firmware upgrade worker.  It will power up the chip,
	 * check the version, and either flash new firmware asynchronously or
	 * fall straight through to scheduling info_work — all without blocking
	 * the probe path.
	 */
	queue_work(system_freezable_wq, &lt7911->fw_upgrade_work);
	return rc;
}

static int lt7911uxc_remove(struct platform_device *pdev)
{
	struct lt7911uxc_data *lt7911 = dev_get_drvdata(&pdev->dev);

	if (lt7911) {
		/*
		 * Disable the GPIO0 IRQ first so no new work items are queued
		 * after cancel_work_sync() returns.
		 */
		if (lt7911->lt7911_gpio0_irq > 0)
			disable_irq(lt7911->lt7911_gpio0_irq);
		cancel_delayed_work_sync(&lt7911->info_work);
		cancel_work_sync(&lt7911->fw_upgrade_work);
		cancel_work_sync(&lt7911->dpalt_work);
		atomic_set(&lt7911->int_event_cnt, 0);
		if (lt7911_sysfs_registered) {
			sysfs_remove_group(&lt7911->dev->kobj, &lt7911_attr_group);
			lt7911_sysfs_registered = false;
		}
		if (lt7911->amclient)
			altmode_deregister_client(lt7911->amclient);
		mutex_lock(&lt7911->device_lock);
		lt7911_power_down(lt7911);
		mutex_unlock(&lt7911->device_lock);
		cci_util_lt7911_put_device(lt7911->cci_handle);
		lt7911->cci_handle = NULL;
		usbmux_put_device(lt7911->usbmux_handle);
		lt7911->usbmux_handle = NULL;
	}
	return 0;
}

static int lt7911uxc_suspend(struct device *dev)
{
	struct lt7911uxc_data *lt7911 = dev_get_drvdata(dev);

	if (!lt7911)
		return 0;

	if (atomic_read(&lt7911->fw_upgrade_in_progress)) {
		dev_warn(dev, "Suspend aborted: firmware upgrade in progress\n");
		return -EBUSY;
	}

	mutex_lock(&lt7911->device_lock);
	if (lt7911->cci_handle)
		cci_util_lt7911_release_cci(lt7911->cci_handle);
	mutex_unlock(&lt7911->device_lock);

	return 0;
}

static int lt7911uxc_resume(struct device *dev)
{
	return 0;
}

static const struct dev_pm_ops lt7911uxc_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(lt7911uxc_suspend, lt7911uxc_resume)
		SET_RUNTIME_PM_OPS(lt7911uxc_suspend, lt7911uxc_resume, NULL)
};

static const struct of_device_id lt7911uxc_id_table[] = {
	{ .compatible = "lontium,lt7911uxc",},
	{ },
};

MODULE_DEVICE_TABLE(of, lt7911uxc_id_table);

static struct platform_driver lt7911uxc_driver = {
	.driver = {
		.name = "lt7911uxc",
		.of_match_table = of_match_ptr(lt7911uxc_id_table),
		.pm = &lt7911uxc_pm_ops,
	},
	.probe = lt7911uxc_probe,
	.remove = lt7911uxc_remove,
};

module_platform_driver(lt7911uxc_driver);
MODULE_DESCRIPTION("Lontium LT7911UXC DP->CSI driver");
MODULE_ALIAS("platform:lt7911uxc");
MODULE_LICENSE("GPL");
