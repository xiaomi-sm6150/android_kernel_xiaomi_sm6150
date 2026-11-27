/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#if !defined(_LONTIUM_LT7911UXC_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _LONTIUM_LT7911UXC_TRACE_H

#include <linux/tracepoint.h>

#undef  TRACE_SYSTEM
#define TRACE_SYSTEM lontium_lt7911uxc

/**
 * lt7911_dpin_connected - fired when a DPIN cable-connect payload is received.
 *
 * @port_index:  altmode port index from the PAN payload
 * @lanes:       number of DP lanes negotiated (2 or 4)
 * @orientation: cable orientation (0 or 1)
 */
TRACE_EVENT(lt7911_dpin_connected,
	TP_PROTO(u8 port_index, int lanes, int orientation),
	TP_ARGS(port_index, lanes, orientation),
	TP_STRUCT__entry(
		__field(u8,  port_index)
		__field(int, lanes)
		__field(int, orientation)
	),
	TP_fast_assign(
		__entry->port_index  = port_index;
		__entry->lanes       = lanes;
		__entry->orientation = orientation;
	),
	TP_printk(
		"DPIN cable connected: port_index=%u lanes=%d orientation=%d",
		__entry->port_index, __entry->lanes, __entry->orientation
	)
);

/**
 * lt7911_attention_ack - fired just before the Attention Message ACK is sent
 *                        to the ADSP PD via altmode_send_data().
 *
 * @port_index: altmode port index the ACK is addressed to
 * @msg_type:   PAN message type (DPIN_SEND_ATTENTION = 0x13)
 */
TRACE_EVENT(lt7911_attention_ack,
	TP_PROTO(u8 port_index, u8 msg_type),
	TP_ARGS(port_index, msg_type),
	TP_STRUCT__entry(
		__field(u8, port_index)
		__field(u8, msg_type)
	),
	TP_fast_assign(
		__entry->port_index = port_index;
		__entry->msg_type   = msg_type;
	),
	TP_printk(
		"Sending Attention Message ACK sent to ADSP PD: port_index=%u msg_type=0x%02x",
		__entry->port_index, __entry->msg_type
	)
);

/**
 * lt7911_gpio0_irq - fired at the top of the GPIO0 hard-IRQ handler.
 *
 * @irq:       Linux IRQ number that fired
 * @event_cnt: value of int_event_cnt *after* the atomic increment
 */
TRACE_EVENT(lt7911_gpio0_irq,
	TP_PROTO(int irq, int event_cnt),
	TP_ARGS(irq, event_cnt),
	TP_STRUCT__entry(
		__field(int, irq)
		__field(int, event_cnt)
	),
	TP_fast_assign(
		__entry->irq       = irq;
		__entry->event_cnt = event_cnt;
	),
	TP_printk(
		"GPIO0 IRQ fired: irq=%d event_cnt=%d",
		__entry->irq, __entry->event_cnt
	)
);

/**
 * lt7911_uevent_sent - fired immediately before kobject_uevent_env() is
 *                      called in lt7911_notify_event().
 *
 * @irq:    interrupt/state value read from the LT7911 registers
 *          (0=not ready, 1=video ready, 2=audio ready, 3=both, >3=HDR)
 * @width:  active horizontal resolution in pixels
 * @height: active vertical resolution in lines
 * @fps:    frame-rate in units of 1/100 Hz  (e.g. 6000 == 60.00 Hz)
 * @format: colour format code (0=YUV422_8b, 1=YUV422_10b,
 *                               2=RGB888_8b, 3=YUV420)
 * @afreq:  audio sample frequency in kHz (0 when no audio)
 * @ach:    number of audio channels (0 when no audio)
 */
TRACE_EVENT(lt7911_uevent_sent,
	TP_PROTO(int irq, int width, int height, int fps,
		 int format, int afreq, int ach),
	TP_ARGS(irq, width, height, fps, format, afreq, ach),
	TP_STRUCT__entry(
		__field(int, irq)
		__field(int, width)
		__field(int, height)
		__field(int, fps)
		__field(int, format)
		__field(int, afreq)
		__field(int, ach)
	),
	TP_fast_assign(
		__entry->irq    = irq;
		__entry->width  = width;
		__entry->height = height;
		__entry->fps    = fps;
		__entry->format = format;
		__entry->afreq  = afreq;
		__entry->ach    = ach;
	),
	TP_printk(
		"UEvent sent: irq=%d w=%d h=%d fps=%d.%02d format=%d afreq=%d ach=%d",
		__entry->irq,
		__entry->width,
		__entry->height,
		__entry->fps / 100, __entry->fps % 100,
		__entry->format,
		__entry->afreq,
		__entry->ach
	)
);

#endif /* _LONTIUM_LT7911UXC_TRACE_H */

#undef  TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .

#undef  TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE lontium-lt7911uxc-trace

/* Must be outside the header guard */
#include <trace/define_trace.h>
