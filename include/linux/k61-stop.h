/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DEBUG (bring-up): "k61.stop=init" test mode, see init/main.c. While it is
 * set, first-stage init may only mount block filesystems read-only and may
 * not open block devices for writing, and PID 1 executing /system/bin/init
 * (the end of first stage) panics.
 */
#ifndef _LINUX_K61_STOP_H
#define _LINUX_K61_STOP_H

#include <linux/types.h>

extern bool k61_stop_init;

#endif
