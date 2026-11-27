/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * All rights reserved.
 */

#ifndef __USBMUX_PS8822_H__
#define __USBMUX_PS8822_H__

#include <linux/types.h>

/* PS8822 I2C Page Addresses (7-bit) */
#define PS8822_PAGE0_ADDR               0x20  /* Page0 write address */
#define PS8822_PAGE1_ADDR               0x22  /* Page1 write address */
#define PS8822_PAGE2_ADDR               0x24  /* Page2 write address */

/* Page0 Registers */
#define PS8822_P0_MODE_CTRL             0x01  /* Mode control register */
#define PS8822_P0_HPD_CTRL              0x02  /* HPD control register */
#define PS8822_P0_CHIP_ID0              0x06  /* Device ID 'P' */
#define PS8822_P0_CHIP_ID1              0x07  /* Device ID 'S' */
#define PS8822_P0_CHIP_ID2              0x08  /* Device ID '8' */
#define PS8822_P0_CHIP_ID3              0x09  /* Device ID '8' */
#define PS8822_P0_CHIP_ID4              0x0A  /* Device ID '2' */
#define PS8822_P0_CHIP_ID5              0x0B  /* Device ID '2' */

/* Page0.0x01 Mode Control Register Bits */
#define MODE_CTRL_DP_ENABLE             0x80  /* Bit7: DP Alt Mode enable */
#define MODE_CTRL_USB_ENABLE            0x40  /* Bit6: USB mode enable */
#define MODE_CTRL_FLIP_ENABLE           0x20  /* Bit5: Flipping mode enable */
#define MODE_CTRL_DP_E_ENABLE           0x10  /* Bit4: DP E Mode enable */

/* Mode Control Base Values (Page0.0x01) - without flip bit */
#define MODE_BASE_SAFE                  0x00  /* 00XX: Standby mode */
#define MODE_BASE_USB_ONLY              0x40  /* 010X: USB only (SS1 channels) */
#define MODE_BASE_4LANE_DP_C            0x80  /* 1000: 4-lane DP Mode C (ML0 on TX2) */
#define MODE_BASE_4LANE_DP_E            0x90  /* 1001: 4-lane DP Mode E (ML0 on RX1, p/n swapped) */
#define MODE_BASE_USB_2LANE_DP          0xC0  /* 110X: 1-port USB + 2-lane DP Mode D (ML0 on TX2) */

/* Legacy defines for compatibility */
#define MODE_SAFE                       MODE_BASE_SAFE
#define MODE_USB_ONLY                   MODE_BASE_USB_ONLY

/* Page0.0x02 HPD Control Register Bits */
#define HPD_CTRL_DISABLE_PIN            0x80  /* Bit7: Disable HPD_IN pin */
#define HPD_CTRL_STATUS                 0x40  /* Bit6: HPD status (when pin disabled) */

/* I2C Message Lengths */
#define I2C_WRITE_MSG_LEN               2
#define I2C_ADDR_MSG_LEN                1
#define I2C_DATA_MSG_LEN                1
#define I2C_WRITE_MSG_COUNT             1
#define I2C_READ_MSG_COUNT              2

/* Chip ID Constants */
#define CHIP_ID_BYTES                   6
#define CHIP_ID_STR_LEN                 8  /* 6 chars + null terminator + 1 spare */
#define CHIP_ID_FORMAT                  "%c%c%c%c%c%c"

/* Delays */
#define MODE_SWITCH_DELAY_MIN           5000
#define MODE_SWITCH_DELAY_MAX           7000
#define CHIP_INIT_DELAY_MS              50

/* Other Constants */
#define DECIMAL_BASE                    10
#define ORIENTATION_COUNT               2

/*
 * UCSI vendor commands used for DP-IN sink-only and CC reconnect control.
 * Bit layout common to all three commands:
 *   [7:0]   Command opcode
 *   [15:8]  DataLength = 0
 *   [22:16] ConnectorNumber (1-indexed; Port2 = ADSP Port[1] = connector 2)
 *   [24]    Mode bit (command-specific: SourceOnly / SinkOnly enable flag)
 */
#define UCSI_CMD_SET_SOURCEONLY         0xF3
#define UCSI_CMD_SET_SINKONLY           0xF4
#define UCSI_CMD_CC_RECONNECT           0xF5

#define UCSI_SOURCEONLY_CONNECTOR_SHIFT 16
#define UCSI_SINKONLY_CONNECTOR_SHIFT   16
#define UCSI_RECONNECT_CONNECTOR_SHIFT  16

#define UCSI_SOURCEONLY_BIT             24
#define UCSI_SINKONLY_BIT               24

/* Port2 = ADSP Port[1] = UCSI connector 2 (1-indexed) */
#define UCSI_SOURCEONLY_CONNECTOR_NUM   2
#define UCSI_SINKONLY_CONNECTOR_NUM     2
#define UCSI_RECONNECT_CONNECTOR_NUM    2

/* Compatibility modes for DP-IN port (SourceOnly control) */
enum usbmux_compat_mode {
	USBMUX_COMPAT_NONE = 0,		/* Normal mode - no SourceOnly */
	USBMUX_COMPAT_SWITCH,		/* Switch mode - enable SourceOnly for Port2 */
	USBMUX_COMPAT_MAX
};

/* Device Operation Modes */
enum usbmux_mode {
	USBMUX_MODE_SAFE = 0,             /* Safe mode - all disabled */
	USBMUX_MODE_USB3_ONLY,            /* USB3 only mode */
	USBMUX_MODE_DP4_LANE,             /* 4-lane DP mode */
	USBMUX_MODE_USB3_2LANE_DP2,       /* 1-port USB + 2-lane DP (default) */
	USBMUX_MODE_MAX
};

/* Device State */
struct usbmux_ps8822_data {
	struct i2c_client *client;
	struct device *dev;
	struct mutex lock;

	/* GPIO controls */
	struct gpio_desc *switch_gpio;  /* Mode switch control (GPIO 69) */
	struct gpio_desc *vdd1v2_gpio;  /* 1.2V power supply control (GPIO 74) */
	struct gpio_desc *vdd3v3_gpio;  /* 3.3V power supply control (GPIO 73) */
	struct gpio_desc *enable_gpio;  /* Chip enable control (GPIO 76) */
	struct gpio_desc *hpd_gpio;     /* Hot plug detect (GPIO 75) */

	/* Current state */
	enum usbmux_mode current_mode;
	enum usbmux_compat_mode compat_mode;    /* DP-IN compat mode (SourceOnly) */
	bool orientation_flip;
	bool hpd_state;
	bool dp_state;    /* DP state for sink-only mode control */
};

/**
 * struct usbmux_handle - opaque client handle for the usbmux_ps8822 driver.
 *
 * Clients treat this as an opaque cookie.  The internal layout is private
 * to usbmux_ps8822.c.
 *
 * Usage model
 * -----------
 * Clients must first obtain a handle via usbmux_get_device() and pass it as
 * the first argument to every other API.  The handle is valid only while the
 * driver is bound; clients must call usbmux_put_device() when they are done
 * (e.g. in their own remove() callback).
 *
 * usbmux_get_device() returns NULL if the driver has not yet probed or has
 * already been removed, giving callers a natural "not ready" signal without
 * any risk of operating on uninitialised state.
 */
struct usbmux_handle;

/**
 * usbmux_get_device - obtain a handle to the usbmux_ps8822 device.
 *
 * Returns a non-NULL handle when the driver has successfully probed, or
 * NULL if the device is not yet available or has been removed.  The caller
 * must release the handle with usbmux_put_device() when finished.
 */
struct usbmux_handle *usbmux_get_device(void);

/**
 * usbmux_put_device - release a handle obtained from usbmux_get_device.
 * @handle: handle to release (may be NULL, in which case this is a no-op)
 */
void usbmux_put_device(struct usbmux_handle *handle);

void usbmux_sethpd(struct usbmux_handle *handle, bool hpd);
void usbmux_setmode(struct usbmux_handle *handle, int lanes, int orientation);
int  usbmux_setdpstate(struct usbmux_handle *handle, bool enable);

#endif /* __USBMUX_PS8822_H__ */
