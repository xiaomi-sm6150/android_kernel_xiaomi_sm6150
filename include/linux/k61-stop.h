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
#include <linux/string.h>

extern bool k61_stop_init;
/* k61.stop=ro: like =init, but refuse with an error instead of panicking,
 * refuse every block-device mount, and do not stop at the exec of init. */
extern bool k61_stop_ro;
/* k61.stop=sys: like =ro, but allow read-only mounts of erofs, or of any
 * filesystem whose block device is itself read-only (dm-linear/dm-verity
 * logical partitions), so first-stage init can mount system/vendor/... */
extern bool k61_stop_sys;

void k61_efs_scratch(const char **names, int n);

struct block_device;
/* k61.rw=<name>,...: internal partitions that may still be written */
bool k61_rw_allowed(struct block_device *bdev);

/*
 * The phone's own storage, which the k61.stop modes must never write:
 * eMMC (mmcblk*, violet) and UFS (sd*, sdmmagpie). Device-mapper targets
 * on top of it are covered too, since dm opens these for its tables.
 */
static inline bool k61_internal_disk(const char *disk_name)
{
	return !strncmp(disk_name, "mmcblk", 6) || !strncmp(disk_name, "sd", 2);
}

#endif
