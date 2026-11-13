/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * Copyright (c) 2011-2014 The Linux Foundation. All rights reserved.
 * Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __USF_H__
#define __USF_H__

#include <linux/types.h>
#include <linux/ioctl.h>

#define USF_IOCTL_MAGIC 'U'

#define US_SET_TX_INFO   _IOW(USF_IOCTL_MAGIC, 0, \
				struct us_tx_info_type)
#define US_START_TX      _IO(USF_IOCTL_MAGIC, 1)
#define US_GET_TX_UPDATE _IOWR(USF_IOCTL_MAGIC, 2, \
				struct us_tx_update_info_type)
#define US_SET_RX_INFO   _IOW(USF_IOCTL_MAGIC, 3, \
				struct us_rx_info_type)
#define US_SET_RX_UPDATE _IOWR(USF_IOCTL_MAGIC, 4, \
				struct us_rx_update_info_type)
#define US_START_RX      _IO(USF_IOCTL_MAGIC, 5)

#define US_STOP_TX      _IO(USF_IOCTL_MAGIC, 6)
#define US_STOP_RX      _IO(USF_IOCTL_MAGIC, 7)

#define US_SET_DETECTION _IOWR(USF_IOCTL_MAGIC, 8, \
				struct us_detect_info_type)

#define US_GET_VERSION  _IOWR(USF_IOCTL_MAGIC, 9, \
				struct us_version_info_type)

#define US_SET_TX_STREAM_PARAM   _IOW(USF_IOCTL_MAGIC, 10, \
				struct us_stream_param_type)
#define US_GET_TX_STREAM_PARAM  _IOWR(USF_IOCTL_MAGIC, 11, \
				struct us_stream_param_type)
#define US_SET_RX_STREAM_PARAM   _IOW(USF_IOCTL_MAGIC, 12, \
				struct us_stream_param_type)
#define US_GET_RX_STREAM_PARAM  _IOWR(USF_IOCTL_MAGIC, 13, \
				struct us_stream_param_type)

/* Special timeout values */
#define USF_NO_WAIT_TIMEOUT	0x00000000
/* Infinitive */
#define USF_INFINITIVE_TIMEOUT	0xffffffff
/* Default value, used by the driver */
#define USF_DEFAULT_TIMEOUT	0xfffffffe

/* US detection place (HW|FW) */
enum us_detect_place_enum {
/* US is detected in HW */
	US_DETECT_HW,
/* US is detected in FW */
	US_DETECT_FW
};

/* US detection mode */
enum us_detect_mode_enum {
/* US detection is disabled */
	US_DETECT_DISABLED_MODE,
/* US detection is enabled in continue mode */
	US_DETECT_CONTINUE_MODE,
/* US detection is enabled in one shot mode */
	US_DETECT_SHOT_MODE
};

/* Encoder (TX), decoder (RX) supported US data formats */
#define USF_POINT_EPOS_FORMAT	0
#define USF_RAW_FORMAT		1

/* Indexes of event types, produced by the calculators */
#define USF_TSC_EVENT_IND      0
#define USF_TSC_PTR_EVENT_IND  1
#define USF_MOUSE_EVENT_IND    2
#define USF_KEYBOARD_EVENT_IND 3
#define USF_TSC_EXT_EVENT_IND  4
#define USF_MAX_EVENT_IND      5

/* Types of events, produced by the calculators */
#define USF_NO_EVENT 0
#define USF_TSC_EVENT      (1 << USF_TSC_EVENT_IND)
#define USF_TSC_PTR_EVENT  (1 << USF_TSC_PTR_EVENT_IND)
#define USF_MOUSE_EVENT    (1 << USF_MOUSE_EVENT_IND)
#define USF_KEYBOARD_EVENT (1 << USF_KEYBOARD_EVENT_IND)
#define USF_TSC_EXT_EVENT  (1 << USF_TSC_EXT_EVENT_IND)
#define USF_ALL_EVENTS         (USF_TSC_EVENT |\
				USF_TSC_PTR_EVENT |\
				USF_MOUSE_EVENT |\
				USF_KEYBOARD_EVENT |\
				USF_TSC_EXT_EVENT)

/* min, max array dimension */
#define MIN_MAX_DIM 2

/* coordinates (x,y,z) array dimension */
#define COORDINATES_DIM 3

/* tilts (x,y) array dimension */
#define TILTS_DIM 2

/* Max size of the client name */
#define USF_MAX_CLIENT_NAME_SIZE	20

/* Max number of the ports (mics/speakers) */
#define USF_MAX_PORT_NUM                8

/* Info structure common for TX and RX */
struct us_xx_info_type {
/* Input:  general info */
/* Name of the client - event calculator */
	const char __user *client_name;
/* Selected device identification, accepted in the kernel's CAD */
	__u32 dev_id;
/* 0 - point_epos type; (e.g. 1 - gr_mmrd) */
	__u32 stream_format;
/* Required sample rate in Hz */
	__u32 sample_rate;
/* Size of a buffer (bytes) for US data transfer between the module and USF */
	__u32 buf_size;
/* Number of the buffers for the US data transfer */
	__u16 buf_num;
/* Number of the microphones (TX) or speakers(RX) */
	__u16 port_cnt;
/* Microphones(TX) or speakers(RX) indexes in their enumeration */
	__u8  port_id[USF_MAX_PORT_NUM];
/* Bits per sample 16 or 32 */
	__u16 bits_per_sample;
/* Input:  Transparent info for encoder in the LPASS */
/* Parameters data size in bytes */
	__u16 params_data_size;
/* Pointer to the parameters */
	__u8 __user *params_data;
/* Max size of buffer for get and set parameter */
	__u32 max_get_set_param_buf_size;
};

struct us_input_info_type {
	/* Touch screen dimensions: min & max;for input module */
	int tsc_x_dim[MIN_MAX_DIM];
	int tsc_y_dim[MIN_MAX_DIM];
	int tsc_z_dim[MIN_MAX_DIM];
	/* Touch screen tilt dimensions: min & max;for input module */
	int tsc_x_tilt[MIN_MAX_DIM];
	int tsc_y_tilt[MIN_MAX_DIM];
	/* Touch screen pressure limits: min & max; for input module */
	int tsc_pressure[MIN_MAX_DIM];
	/* The requested buttons bitmap */
	__u16 req_buttons_bitmap;
	/* Bitmap of types of events (USF_X_EVENT), produced by calculator */
	__u16 event_types;
	/* Bitmap of types of events from devs, conflicting with USF */
	__u16 conflicting_event_types;
};

struct us_tx_info_type {
	/* Common info */
	struct us_xx_info_type us_xx_info;
	/* Info specific for TX*/
	struct us_input_info_type input_info;
};

struct us_rx_info_type {
	/* Common info */
	struct us_xx_info_type us_xx_info;
	/* Info specific for RX*/
};

struct point_event_type {
/* Pen coordinates (x, y, z) in units, defined by <coordinates_type>  */
	int coordinates[COORDINATES_DIM];
	/* {x;y}  in transparent units */
	int inclinations[TILTS_DIM];
/* [0-1023] (10bits); 0 - pen up */
	__u32 pressure;
/* Bitmap for button state. 1 - down, 0 - up */
	__u16 buttons_state_bitmap;
};

/* Mouse buttons, supported by USF */
#define USF_BUTTON_LEFT_MASK   1
#define USF_BUTTON_MIDDLE_MASK 2
#define USF_BUTTON_RIGHT_MASK  4
struct mouse_event_type {
/* The mouse relative movement (dX, dY, dZ) */
	int rels[COORDINATES_DIM];
/* Bitmap of mouse buttons states: 1 - down, 0 - up; */
	__u16 buttons_states;
};

struct key_event_type {
/*  Calculated MS key- see input.h. */
	__u32 key;
/* Keyboard's key state: 1 - down, 0 - up; */
	__u8 key_state;
};

struct usf_event_type {
/* Event sequence number */
	__u32 seq_num;
/* Event generation system time */
	__u32 timestamp;
/* Destination input event type index (e.g. touch screen, mouse, key) */
	__u16 event_type_ind;
	union {
		struct point_event_type point_event;
		struct mouse_event_type mouse_event;
		struct key_event_type   key_event;
	} event_data;
};

struct us_tx_update_info_type {
/* Input  general: */
/* Number of calculated events */
	__u16 event_counter;
/* Calculated events or NULL */
	struct usf_event_type __user *event;
/* Pointer (read index) to the end of available region */
/* in the shared US data memory */
	__u32 free_region;
/* Time (sec) to wait for data or special values: */
/* USF_NO_WAIT_TIMEOUT, USF_INFINITIVE_TIMEOUT, USF_DEFAULT_TIMEOUT */
	__u32 timeout;
/* Events (from conflicting devs) to be disabled/enabled */
	__u16 event_filters;

/* Input  transparent data: */
/* Parameters size */
	__u16 params_data_size;
/* Pointer to the parameters */
	__u8 __user *params_data;
/* Output parameters: */
/* Pointer (write index) to the end of ready US data region */
/* in the shared memory */
	__u32 ready_region;
};

struct us_rx_update_info_type {
/* Input  general: */
/* Pointer (write index) to the end of ready US data region */
/* in the shared memory */
	__u32 ready_region;
/* Input  transparent data: */
/* Parameters size */
	__u16 params_data_size;
/* pPointer to the parameters */
	__u8 __user *params_data;
/* Output parameters: */
/* Pointer (read index) to the end of available region */
/* in the shared US data memory */
	__u32 free_region;
};

struct us_detect_info_type {
/* US detection place (HW|FW) */
/* NA in the Active and OFF states */
	enum us_detect_place_enum us_detector;
/* US detection mode */
	enum us_detect_mode_enum  us_detect_mode;
/* US data dropped during this time (msec) */
	__u32 skip_time;
/* Transparent data size */
	__u16 params_data_size;
/* Pointer to the transparent data */
	__u8 __user *params_data;
/* Time (sec) to wait for US presence event */
	__u32 detect_timeout;
/* Out parameter: US presence */
	_Bool is_us;
};

struct us_version_info_type {
/* Size of memory for the version string */
	__u16 buf_size;
/* Pointer to the memory for the version string */
	char __user *pbuf;
};

struct us_stream_param_type {
/* Id of module */
	__u32 module_id;
/* Id of parameter */
	__u32 param_id;
/* Size of memory of the parameter buffer */
	__u32 buf_size;
/* Pointer to the memory of the parameter buffer */
	__u8 __user *pbuf;
};

#endif /* __USF_H__ */
