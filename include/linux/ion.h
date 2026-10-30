/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * In-kernel side of the /dev/ion compat (drivers/dma-buf/heaps/
 * qcom_legacy_ion.c) for the 5.4 vendor drivers carried forward to 6.1.
 */

#ifndef _ION_KERNEL_H
#define _ION_KERNEL_H

#include <linux/dma-buf.h>
#include <linux/err.h>
#include <uapi/linux/ion.h>

#if IS_ENABLED(CONFIG_QCOM_LEGACY_ION)
struct dma_buf *ion_alloc(size_t len, unsigned int heap_id_mask,
			  unsigned int flags);
#else
static inline struct dma_buf *ion_alloc(size_t len, unsigned int heap_id_mask,
					unsigned int flags)
{
	return ERR_PTR(-ENOMEM);
}
#endif

#endif /* _ION_KERNEL_H */
