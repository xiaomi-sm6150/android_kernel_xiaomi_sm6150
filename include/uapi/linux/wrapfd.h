/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Wrapfd UAPI
 *
 * Copyright (C) 2025 Google, Inc.
 */

#ifndef _UAPI_WRAPFD_H
#define _UAPI_WRAPFD_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct wrapfd_wrap {
	__u32 fd;		/* [in] file to wrap */
	__u32 prot;		/* [in] protection bits */
	__u64 reserved;
};

#define WRAPFD_CONTENT_EMPTY	0
#define WRAPFD_CONTENT_RDONLY	1
#define WRAPFD_CONTENT_RDWR	2

/* wrapfd_monitor.events bit values */
#define WRAPFD_EVENT_MAP	(1 << 0)
#define WRAPFD_EVENT_UNMAP	(1 << 1)
#define WRAPFD_EVENT_OWN	(1 << 2)
#define WRAPFD_EVENT_UNOWN	(1 << 3)
#define WRAPFD_EVENT_LOAD	(1 << 4)
#define WRAPFD_EVENT_LOAD_DONE	(1 << 5)

#define WRAPFD_EVENTS_MASK	(WRAPFD_EVENT_MAP | WRAPFD_EVENT_UNMAP | \
			WRAPFD_EVENT_OWN | WRAPFD_EVENT_UNOWN | \
			WRAPFD_EVENT_LOAD | WRAPFD_EVENT_LOAD_DONE)

struct wrapfd_get_state {
	__u32 state;		/* [out] wrapfd content state */
	__u32 pad;
	__u64 reserved;
};

struct wrapfd_load {
	__u32 fd;		/* [in] file to load */
	__u32 pad;
	__u64 file_offs;	/* [in] file offset */
	__u64 buf_offs;		/* [in] buffer offset */
	__u64 len;		/* [in] number of bytes to load */
	__u64 reserved;
};

struct wrapfd_rewrap {
	__u32 prot;		/* [in] protection bits */
	__u32 pad;
	__u64 reserved;
};

struct wrapfd_add_monitor {
	__u32 fd; /* [in] wrapfd to add a monitor for */
	__u32 events; /* [in] events to start monitoring */
	__u64 pad; /* must be 0 */
};

struct wrapfd_del_monitor {
	__u64 id; /* [in] ID from WRAPFD_DEV_IOC_ADD_MONITOR of monitor to delete */
	__u32 events; /* [in] events to stop monitoring. pending events are discarded. */
	__u32 pad; /* must be 0 */
};

struct wrapfd_monitors {
	__u64 len;		/* [in] number of elements in arr and results */
	__u64 arr;		/* [in] array of struct wrapfd_[add|del]_monitor */
	/*
	 * [out] array of 64-bit signed integers. A negative value denotes an error in any case.
	 *
	 * When used with a WRAPFD_DEV_IOC_ADD_MONITOR call, a non-negative value denotes a monitor
	 * ID, which is used to identify the monitor that was created for the wrapfd specified in
	 * the corresponding wrapfd_add_monitor structure.
	 *
	 * When used with a WRAPFD_DEV_IOC_DEL_MONITOR call, 0 is returned to indicate success.
	 * -EAGAIN can be returned to indicate that the monitor could not be deleted, and the
	 * caller should try again.
	 *
	 * Notes about monitor IDs and monitor lifetimes:
	 * - IDs within a single /dev/wrapfd instance are 64-bit values and are never reused across
	 *   wrap objects.
	 * - If a monitor is added several times for the same wrap object, each addition will always
	 *   return the same ID, unless the ID is deleted via a call to WRAPFD_DEV_IOC_DEL_MONITOR
	 *   that removes monitoring of all events for that ID, in which case a new ID will be
	 *   allocated on the next WRAPFD_DEV_IOC_ADD_MONITOR call.
	 * - A monitor's lifetime is tied to either the lifetime of the wrap object it's associated
	 *   with or the /dev/wrapfd instance it's associated with.
	 * - In the case where a wrap object is destroyed, only its corresponding monitor is
	 *   destroyed.
	 * - In the case where the /dev/wrapfd instance is destroyed, all monitors associated with
	 *   that instance are destroyed.
	 * - For a given /dev/wrapfd instance and a wrap object, there is at most one (wrap, id)
	 *   pair.
	 */
	__u64 results;		/* [out] array of 64-bit signed integers. */
};

struct wrapfd_add_monitors {
	struct wrapfd_monitors monitors;
	__u64 reserved;
};

struct wrapfd_del_monitors {
	struct wrapfd_monitors monitors;
	__u64 reserved;
};

struct wrapfd_event {
	__u64 id; /* [out] ID from WRAPFD_DEV_IOC_ADD_MONITOR of monitor that generated an event */
	__u64 timestamp; /* [out] the timestamp--using CLOCK_MONOTONIC--of the latest event */
	__u32 events; /* [out] a bitmask expressing what events happened since the last query */
	__u32 pad;
};

struct wrapfd_events {
	__u64 len;		/* [in] number of elements in arr */
	__u64 arr;		/* [in / out] array of struct wrapfd_event */
	__u64 count;		/* [out] number of events reported in arr */
};

struct wrapfd_repopulate {
	__u32 wrapfd;		/* [in] wrapfd file to populate */
	__u32 fd;		/* [in] file to populate with */
	__u64 reserved;
};

/*
 * The maximum number of monitors to add/delete and events are defined as the maximum amount of
 * structures that fit on the smallest supported page size (4096 bytes).
 *
 * wrapfd_[add|del]_monitor are the same size to avoid having to juggle two maximums based on
 * monitor command type.
 */
#define WRAPFD_MAX_NR_MONITORS	256
#define WRAPFD_MAX_NR_EVENTS	170

/* ioctls for /dev/wrapfd */
#define WRAPFD_DEV_IOC 0xBC

/* ioctl for wrapfd */
enum {
	WRAPFD_DEV_IOC_WRAP	= _IOW(WRAPFD_DEV_IOC, 0, struct wrapfd_wrap),
	WRAPFD_DEV_IOC_GET_STATE	= _IOWR(WRAPFD_DEV_IOC, 1, struct wrapfd_get_state),
	WRAPFD_DEV_IOC_ACQUIRE_OWNERSHIP	= _IO(WRAPFD_DEV_IOC, 2),
	WRAPFD_DEV_IOC_RELEASE_OWNERSHIP	= _IO(WRAPFD_DEV_IOC, 3),
	WRAPFD_DEV_IOC_LOAD	= _IOW(WRAPFD_DEV_IOC, 4, struct wrapfd_load),
	WRAPFD_DEV_IOC_REWRAP	= _IOW(WRAPFD_DEV_IOC, 5, struct wrapfd_rewrap),
	WRAPFD_DEV_IOC_EMPTY	= _IO(WRAPFD_DEV_IOC, 6),
	WRAPFD_DEV_IOC_ALLOW_GUESTS	= _IO(WRAPFD_DEV_IOC, 7),
	WRAPFD_DEV_IOC_PROHIBIT_GUESTS	= _IO(WRAPFD_DEV_IOC, 8),
	WRAPFD_DEV_IOC_ADD_MONITOR	= _IOWR(WRAPFD_DEV_IOC, 9, struct wrapfd_add_monitors),
	WRAPFD_DEV_IOC_DEL_MONITOR	= _IOWR(WRAPFD_DEV_IOC, 10, struct wrapfd_del_monitors),
	WRAPFD_DEV_IOC_QUERY_EVENTS	= _IOWR(WRAPFD_DEV_IOC, 11, struct wrapfd_events),
	WRAPFD_DEV_IOC_REPOPULATE	= _IOW(WRAPFD_DEV_IOC, 12, struct wrapfd_repopulate),
};

#endif /* _UAPI_WRAPFD_H */
