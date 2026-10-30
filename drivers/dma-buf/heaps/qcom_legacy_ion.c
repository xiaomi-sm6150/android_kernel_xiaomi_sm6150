// SPDX-License-Identifier: GPL-2.0-only
/*
 * /dev/ion for an Android 11 vendor userspace on top of the qcom DMA-BUF
 * heaps.
 *
 * Vendor blobs built for a 5.4 kernel allocate shared memory through the
 * VNDK v30 libion, which only knows /dev/ion. On 5.4 that was GKI modular
 * ION (ABI version 2) with the msm heaps: ION_IOC_ALLOC with a mask of the
 * single-bit msm heap IDs, ION_IOC_HEAP_QUERY and ION_IOC_ABI_VERSION. There
 * is no ION_IOC_FREE, which is how libion tells this ABI apart from the old
 * handle based one.
 *
 * Each 5.4 heap ID is backed by the qcom DMA-BUF heap that 6.1 vendor
 * userspace would use for the same purpose, so the buffers are the same
 * kind (e.g. mem-buf aware, as qseecom requires for its shmbridge).
 */

#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/fdtable.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/ion.h>

#define ION_IOC_MAGIC		'I'
#define ION_IOC_ALLOC		_IOWR(ION_IOC_MAGIC, 0, struct ion_allocation_data)
#define ION_IOC_HEAP_QUERY	_IOWR(ION_IOC_MAGIC, 8, struct ion_heap_query)
#define ION_IOC_ABI_VERSION	_IOR(ION_IOC_MAGIC, 9, __u32)

#define ION_ABI_VERSION		2

/* 5.4 msm_ion.h secure buffer flags */
#define ION_FLAG_CP_PIXEL	BIT(19)
#define ION_FLAG_CP_NON_PIXEL	BIT(20)

struct legacy_ion_heap {
	unsigned int id;		/* bit in heap_id_mask, i.e. __ffs() of the 5.4 ID */
	const char *name;		/* 5.4 heap name, as reported by HEAP_QUERY */
	unsigned int type;		/* reported ION heap type */
	const char *heap;		/* backing DMA-BUF heap */
	const char *uncached_heap;	/* backing heap without ION_FLAG_CACHED */
	const char *pixel_heap;		/* backing heap for ION_FLAG_CP_PIXEL */
	const char *non_pixel_heap;	/* backing heap for ION_FLAG_CP_NON_PIXEL */
};

static const struct legacy_ion_heap legacy_ion_heaps[] = {
	{ .id = 25, .name = "system", .type = ION_HEAP_TYPE_SYSTEM,
	  .heap = "qcom,system", .uncached_heap = "qcom,system-uncached" },
	{ .id = 10, .name = "secure_heap", .type = ION_HEAP_TYPE_SYSTEM,
	  .pixel_heap = "qcom,secure-pixel",
	  .non_pixel_heap = "qcom,secure-non-pixel" },
	{ .id = 7, .name = "qsecom", .type = ION_HEAP_TYPE_DMA,
	  .heap = "qcom,qseecom" },
	{ .id = 6, .name = "user_contig", .type = ION_HEAP_TYPE_DMA,
	  .heap = "qcom,user-contig" },
	{ .id = 5, .name = "audio_ml", .type = ION_HEAP_TYPE_DMA,
	  .heap = "qcom,audio-ml" },
	{ .id = 4, .name = "adsp", .type = ION_HEAP_TYPE_DMA,
	  .heap = "qcom,adsp" },
	{ .id = 3, .name = "display", .type = ION_HEAP_TYPE_DMA,
	  .heap = "qcom,display" },
	{ .id = 1, .name = "qsecom_ta", .type = ION_HEAP_TYPE_DMA,
	  .heap = "qcom,qseecom-ta" },
	/*
	 * The GKI system heap, which ION core gave the ION_HEAP_SYSTEM bit.
	 * libdmabufheap (Codec2) finds it by this name when there is no
	 * "system" DMA-BUF heap.
	 */
	{ .id = 0, .name = "ion_system_heap", .type = ION_HEAP_TYPE_SYSTEM,
	  .heap = "qcom,system", .uncached_heap = "qcom,system-uncached" },
};

static const char *legacy_ion_backing_heap(const struct legacy_ion_heap *h,
					   unsigned int flags)
{
	if (flags & ION_FLAG_CP_PIXEL)
		return h->pixel_heap;
	if (flags & ION_FLAG_CP_NON_PIXEL)
		return h->non_pixel_heap;
	if (!(flags & ION_FLAG_CACHED) && h->uncached_heap)
		return h->uncached_heap;
	return h->heap;
}

static bool legacy_ion_heap_exists(const struct legacy_ion_heap *h)
{
	const char *names[] = { h->heap, h->uncached_heap, h->pixel_heap,
				h->non_pixel_heap };
	struct dma_heap *heap;
	int i;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		if (!names[i])
			continue;
		heap = dma_heap_find(names[i]);
		if (heap) {
			dma_heap_put(heap);
			return true;
		}
	}
	return false;
}

/* As 5.4 ION: try the heaps in the mask from the highest ID down */
static int legacy_ion_alloc(struct ion_allocation_data *data)
{
	const struct legacy_ion_heap *h;
	struct dma_heap *heap;
	const char *name;
	int i, fd, ret = -ENODEV;

	if (!data->len)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(legacy_ion_heaps); i++) {
		h = &legacy_ion_heaps[i];
		if (!(data->heap_id_mask & BIT(h->id)))
			continue;

		name = legacy_ion_backing_heap(h, data->flags);
		heap = name ? dma_heap_find(name) : NULL;
		if (!heap)
			continue;

		fd = dma_heap_bufferfd_alloc(heap, data->len,
					     O_RDWR | O_CLOEXEC, 0);
		dma_heap_put(heap);
		if (fd >= 0) {
			data->fd = fd;
			return 0;
		}
		ret = fd;
	}

	return ret;
}

/**
 * ion_alloc - allocate a dma-buf with a 5.4 ION heap mask and flags
 * @len:		size in bytes
 * @heap_id_mask:	mask of 5.4 msm heap IDs (ION_HEAP())
 * @flags:		5.4 ION flags (ION_FLAG_CACHED, ION_FLAG_CP_*)
 *
 * The kernel counterpart of ION_IOC_ALLOC, with the same heap selection, for
 * the 5.4 drivers that allocate their own buffers (msm_vidc, msm_cvp).
 */
struct dma_buf *ion_alloc(size_t len, unsigned int heap_id_mask,
			  unsigned int flags)
{
	const struct legacy_ion_heap *h;
	struct dma_buf *dbuf = ERR_PTR(-ENODEV);
	struct dma_heap *heap;
	const char *name;
	int i;

	if (!len)
		return ERR_PTR(-EINVAL);

	for (i = 0; i < ARRAY_SIZE(legacy_ion_heaps); i++) {
		h = &legacy_ion_heaps[i];
		if (!(heap_id_mask & BIT(h->id)))
			continue;

		name = legacy_ion_backing_heap(h, flags);
		heap = name ? dma_heap_find(name) : NULL;
		if (!heap)
			continue;

		dbuf = dma_heap_buffer_alloc(heap, len, O_RDWR, 0);
		dma_heap_put(heap);
		if (!IS_ERR(dbuf))
			break;
	}

	return dbuf;
}
EXPORT_SYMBOL(ion_alloc);

static int legacy_ion_query_heaps(struct ion_heap_query *query)
{
	struct ion_heap_data __user *buf = u64_to_user_ptr(query->heaps);
	struct ion_heap_data hdata;
	unsigned int cnt = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(legacy_ion_heaps); i++) {
		const struct legacy_ion_heap *h = &legacy_ion_heaps[i];

		if (!legacy_ion_heap_exists(h))
			continue;

		if (buf) {
			if (cnt >= query->cnt)
				break;

			memset(&hdata, 0, sizeof(hdata));
			strscpy(hdata.name, h->name, sizeof(hdata.name));
			hdata.type = h->type;
			hdata.heap_id = h->id;
			if (copy_to_user(&buf[cnt], &hdata, sizeof(hdata)))
				return -EFAULT;
		}
		cnt++;
	}

	query->cnt = cnt;
	return 0;
}

static long legacy_ion_ioctl(struct file *filp, unsigned int cmd,
			     unsigned long arg)
{
	void __user *uarg = (void __user *)arg;
	union {
		struct ion_allocation_data allocation;
		struct ion_heap_query query;
		u32 abi_version;
	} data;
	unsigned int size = _IOC_SIZE(cmd);
	int ret;

	switch (cmd) {
	case ION_IOC_ALLOC:
	case ION_IOC_HEAP_QUERY:
	case ION_IOC_ABI_VERSION:
		break;
	default:
		return -ENOTTY;
	}

	if (size > sizeof(data))
		return -EINVAL;
	if ((_IOC_DIR(cmd) & _IOC_WRITE) && copy_from_user(&data, uarg, size))
		return -EFAULT;

	switch (cmd) {
	case ION_IOC_ALLOC:
		ret = legacy_ion_alloc(&data.allocation);
		break;
	case ION_IOC_HEAP_QUERY:
		if (data.query.reserved0 || data.query.reserved1 ||
		    data.query.reserved2)
			return -EINVAL;
		ret = legacy_ion_query_heaps(&data.query);
		break;
	default:
		data.abi_version = ION_ABI_VERSION;
		ret = 0;
		break;
	}
	if (ret)
		return ret;

	if (copy_to_user(uarg, &data, size)) {
		if (cmd == ION_IOC_ALLOC)
			close_fd(data.allocation.fd);
		return -EFAULT;
	}
	return 0;
}

static const struct file_operations legacy_ion_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= legacy_ion_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static struct miscdevice legacy_ion_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "ion",
	.fops	= &legacy_ion_fops,
};

/* After the qcom heaps (module_init) have registered */
static int __init legacy_ion_init(void)
{
	return misc_register(&legacy_ion_dev);
}
late_initcall(legacy_ion_init);

MODULE_DESCRIPTION("/dev/ion for Android 11 vendor userspace on qcom DMA-BUF heaps");
MODULE_LICENSE("GPL");
