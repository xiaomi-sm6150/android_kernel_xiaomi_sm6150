/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef UAPI_UFS_IOCTL_H_
#define UAPI_UFS_IOCTL_H_

#include <linux/types.h>

/*
 * UFS query ioctl of the Qualcomm 4.14 kernels, issued on a SCSI device
 * node (sg) of the UFS host. Vendor librpmb (qseecomd) reads the device
 * and RPMB unit descriptors with it.
 */
#define UFS_IOCTL_QUERY			0x5388

/**
 * struct ufs_ioctl_query_data - used to transfer data to and from user via ioctl
 * @opcode: UPIU_QUERY_OPCODE_READ_DESC, _READ_ATTR or _READ_FLAG
 * @idn: descriptor, attribute or flag id
 * @buf_size: bytes available after the header; bytes read on return
 * @buffer: data location
 */
struct ufs_ioctl_query_data {
	__u32 opcode;
	__u8 idn;
	__u16 buf_size;
	__u8 buffer[0];
};

#endif /* UAPI_UFS_IOCTL_H_ */
