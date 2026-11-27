/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef DPIN_CCI_UTIL_H
#define DPIN_CCI_UTIL_H

#include <linux/types.h>
#include <linux/videodev2.h>
#include <media/v4l2-subdev.h>

#define CCI_MAX_CMD_BUFFER 20

/*
 * I2C type / frequency / master enums
 * (mirrors camera_sensor_i2c_type, i2c_freq_mode, cci_i2c_master_t)
 */

enum dpin_cci_util_i2c_type {
	DPIN_CCI_UTIL_I2C_TYPE_INVALID = 0,
	DPIN_CCI_UTIL_I2C_TYPE_BYTE    = 1,
	DPIN_CCI_UTIL_I2C_TYPE_WORD    = 2,
	DPIN_CCI_UTIL_I2C_TYPE_3B      = 3,
	DPIN_CCI_UTIL_I2C_TYPE_DWORD   = 4,
	DPIN_CCI_UTIL_I2C_TYPE_MAX,
};

enum dpin_cci_util_i2c_freq_mode {
	DPIN_CCI_UTIL_I2C_STANDARD_MODE = 0,
	DPIN_CCI_UTIL_I2C_FAST_MODE,
	DPIN_CCI_UTIL_I2C_CUSTOM_MODE,
	DPIN_CCI_UTIL_I2C_FAST_PLUS_MODE,
	DPIN_CCI_UTIL_I2C_MAX_MODES,
};

enum dpin_cci_util_i2c_master {
	DPIN_CCI_UTIL_MASTER_0 = 0,
	DPIN_CCI_UTIL_MASTER_1,
	DPIN_CCI_UTIL_MASTER_MAX,
};

/*
 * Register array / write-setting structs
 * (mirrors cam_sensor_i2c_reg_array, cam_sensor_i2c_reg_setting)
 */

struct dpin_cci_util_i2c_reg_array {
	u32 reg_addr;
	u32 reg_data;
	u32 delay;
	u32 data_mask;
};

struct dpin_cci_util_i2c_reg_setting {
	struct dpin_cci_util_i2c_reg_array *reg_setting;
	u32 size;
	enum dpin_cci_util_i2c_type addr_type;
	enum dpin_cci_util_i2c_type data_type;
	u16 delay;
	u8 *read_buff;
	u32 read_buff_len;
};

/*
 * CCI command enum
 * (mirrors cam_cci_cmd_type)
 */

enum dpin_cci_util_cmd_type {
	DPIN_CCI_UTIL_MSM_CCI_INIT = 0,
	DPIN_CCI_UTIL_MSM_CCI_RELEASE,
	DPIN_CCI_UTIL_MSM_CCI_SET_SID,
	DPIN_CCI_UTIL_MSM_CCI_SET_FREQ,
	DPIN_CCI_UTIL_MSM_CCI_SET_SYNC_CID,
	DPIN_CCI_UTIL_MSM_CCI_I2C_READ,
	DPIN_CCI_UTIL_MSM_CCI_I2C_WRITE,
	DPIN_CCI_UTIL_MSM_CCI_I2C_WRITE_SEQ,
	DPIN_CCI_UTIL_MSM_CCI_I2C_WRITE_BURST,
	DPIN_CCI_UTIL_MSM_CCI_I2C_WRITE_ASYNC,
	DPIN_CCI_UTIL_MSM_CCI_GPIO_WRITE,
	DPIN_CCI_UTIL_MSM_CCI_I2C_WRITE_SYNC,
	DPIN_CCI_UTIL_MSM_CCI_I2C_WRITE_SYNC_BLOCK,
};

/*
 * CCI client struct
 * (mirrors cam_sensor_cci_client)
 */

struct dpin_cci_util_sensor_client {
	struct v4l2_subdev *cci_subdev;
	u32 freq;
	enum dpin_cci_util_i2c_freq_mode i2c_freq_mode;
	enum dpin_cci_util_i2c_master cci_i2c_master;
	u16 sid;
	u16 cid;
	u32 timeout;
	u16 retries;
	u16 id_map;
	u16 cci_device;
};

/*
 * Minimal read config - keeps the union in dpin_cci_util_ctrl the same size
 * as the corresponding union in cam_cci_ctrl
 * (mirrors cam_cci_read_cfg)
 */

struct dpin_cci_util_read_cfg {
	u32 addr;
	u16 addr_type;
	u8 *data;
	u16 num_byte;
	u16 data_type;
};

enum cam_sensor_cmd_buffer_type {
	CAM_SENSOR_CMD_TYPE_I2C_SETTING,
	CAM_SENSOR_CMD_TYPE_QTIMER,
	CAM_SENSOR_CMD_TYPE_FSIN_TRIGGER,
	CAM_SENSOR_CMD_TYPE_TIMER,
	CAM_SENSOR_CMD_TYPE_FRAME_EVENT,
	CAM_SENSOR_CMD_TYPE_SYNC_CMD,
	CAM_SENSOR_CMD_TYPE_DEBUG_CMD,
	CAM_SENSOR_CMD_TYPE_EVENT,
	CAM_SENSOR_CMD_TYPE_INVALID,
};

struct cam_sensor_cmd_sequence {
	enum cam_sensor_cmd_buffer_type cmd_type;
	u32 cmd_flag;
	s32 index;
	void *payload;
};

struct cam_sensor_event_arg_sequence {
	enum cam_sensor_cmd_buffer_type cmd_type;
	s32 index;
	void *payload;
};

struct cam_sensor_trigger_event_info {
	u32 event;
	u32 event_flag;
	u32 event_arg_count;
	struct cam_sensor_event_arg_sequence event_arg_sequence[CCI_MAX_CMD_BUFFER];
	u32 cmd_count;
	struct cam_sensor_cmd_sequence cmd_sequence[CCI_MAX_CMD_BUFFER];
};

struct cam_sensor_cci_event_setting {
	u32 event_count;
	u32 context_id;
	struct cam_sensor_trigger_event_info *trigger_info;
};

struct cam_cci_wait_sync_cfg {
	u16 cid;
	s16 csid;
	u16 line;
	u16 delay;
};

struct cam_cci_gpio_cfg {
	u16 gpio_queue;
	u16 i2c_queue;
};

struct cam_cci_trigger_data {
	u32 csid;
	u32 cid;
	u32 context_id;
	u32 gpio_mask;
};

/*
 * CCI control struct passed to the CCI sub-device ioctl
 * (mirrors cam_cci_ctrl)
 */

struct dpin_cci_util_ctrl {
	s32 status;
	struct dpin_cci_util_sensor_client *cci_info;
	enum dpin_cci_util_cmd_type cmd;
	union {
		struct dpin_cci_util_i2c_reg_setting     cci_i2c_write_cfg;
		struct cam_sensor_cci_event_setting      cci_event_write_cfg;
		struct dpin_cci_util_read_cfg            cci_i2c_read_cfg;
		struct cam_cci_wait_sync_cfg             cci_wait_sync_cfg;
		struct cam_cci_gpio_cfg                  gpio_cfg;
		struct cam_cci_trigger_data              trigger_data;
	} cfg;
};

/*
 * VIDIOC_MSM_CCI_CFG ioctl command
 *
 * Defined as: _IOWR('V', BASE_VIDIOC_PRIVATE + 23, struct cam_cci_ctrl)
 * BASE_VIDIOC_PRIVATE = 192  (linux/videodev2.h)
 *
 * dpin_cci_util_ctrl must be layout-compatible with cam_cci_ctrl because the
 * ioctl number encodes the struct size.
 */

#define DPIN_CCI_UTIL_VIDIOC_MSM_CCI_CFG \
		_IOWR('V', 192 + 23, struct dpin_cci_util_ctrl)

/*
 * LT7911 firmware upgrade public API
 * (implemented in dpin_cci_util.c, consumed by lontium-lt7911uxc.c)
 *
 * Usage model
 * -----------
 * Clients must first obtain a handle via cci_util_lt7911_get_device() and
 * pass it as the first argument to every other API.  The handle is valid
 * only while the driver is bound; clients must call cci_util_lt7911_put_device()
 * when they are done (e.g. in their own remove() callback).
 *
 * cci_util_lt7911_get_device() returns NULL if the driver has not yet probed
 * or has already been removed, giving callers a natural "not ready" signal
 * without any risk of operating on uninitialised state.
 */

#define LT7911UXC_VERSION_NUM   0x4020100
#define LT7911_HDCPKEY_SIZE     32

struct firmware;

/**
 * struct cci_util_handle - opaque client handle for the dpin_cci_util driver.
 *
 * Clients treat this as an opaque cookie.  The internal layout is private
 * to dpin_cci_util.c.
 */
struct cci_util_handle;

/**
 * cci_util_lt7911_get_device - obtain a handle to the CCI-util device.
 *
 * Returns a non-NULL handle when the driver has successfully probed, or
 * NULL if the device is not yet available or has been removed.  The caller
 * must release the handle with cci_util_lt7911_put_device() when finished.
 */
struct cci_util_handle *cci_util_lt7911_get_device(void);

/**
 * cci_util_lt7911_put_device - release a handle obtained from get_device.
 * @handle: handle to release (may be NULL, in which case this is a no-op)
 */
void cci_util_lt7911_put_device(struct cci_util_handle *handle);

uint32_t cci_util_lt7911_get_version(struct cci_util_handle *handle);
int      cci_util_lt7911_read_chip_id(struct cci_util_handle *handle);
int      cci_util_lt7911_read_hdcpkey(struct cci_util_handle *handle, uint8_t *buff, int size);
int      cci_util_lt7911_do_firmware_upgrade(struct cci_util_handle *handle,
					  const struct firmware *fw);
int      cci_util_lt7911_reg_read(struct cci_util_handle *handle, int reg, int *reg_value);
int      cci_util_lt7911_reg_write(struct cci_util_handle *handle,
				   struct dpin_cci_util_i2c_reg_array *reg_cfg, int reg_sz);
int      cci_util_lt7911_reg_read_seq(struct cci_util_handle *handle,
				      u8 reg, u8 *out_buf, u16 num_bytes);
void     cci_util_lt7911_enable_i2c(struct cci_util_handle *handle);
void     cci_util_lt7911_disable_i2c(struct cci_util_handle *handle);
void     cci_util_lt7911_release_cci(struct cci_util_handle *handle);
int      cci_util_lt7911_get_information(struct cci_util_handle *handle,
					 int *irq, int *width, int *height,
					 int *fps, int *format, int *afreq, int *ach);
int      cci_util_lt7911_get_interrupt_type(struct cci_util_handle *handle, int *irq_type);

#endif /* DPIN_CCI_UTIL_H */
