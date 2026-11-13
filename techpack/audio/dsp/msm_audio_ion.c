// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2013-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/dma-mapping.h>
#include <linux/dma-buf.h>
#include <linux/iosys-map.h>
#include <linux/dma-heap.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/export.h>
#include <ipc/apr.h>
#include <dsp/msm_audio_ion.h>
#include <soc/qcom/secure_buffer.h>
#include <linux/of_reserved_mem.h>
#include <linux/qcom_scm.h>

MODULE_IMPORT_NS(DMA_BUF);

#define ION_FLAG_CACHED 1
#define MSM_AUDIO_ION_PROBED (1 << 0)

#define MSM_AUDIO_ION_PHYS_ADDR(alloc_data) \
	alloc_data->table->sgl->dma_address

#define MSM_AUDIO_SMMU_SID_OFFSET 32

enum msm_audio_mem_type {
	MSM_AUDIO_MEM_TYPE_ION,
	MSM_AUDIO_MEM_TYPE_DMA,
};

struct msm_audio_ion_private {
	bool smmu_enabled;
	struct device *cb_dev;
	struct device *cb_cma_dev;
	u8 device_status;
	struct list_head alloc_list;
	struct mutex list_mutex;
	u64 smmu_sid_bits;
	u32 smmu_version;
};

struct msm_audio_alloc_data {
	size_t len;
	struct iosys_map *vmap;
	void *vaddr;
	void *handle;
	struct dma_buf_attachment *attach;
	struct sg_table *table;
	struct list_head list;
	dma_addr_t *paddr;
	enum msm_audio_mem_type type;
};

static struct msm_audio_ion_private msm_audio_ion_data = {0,};

static void msm_audio_ion_add_allocation(
	struct msm_audio_ion_private *msm_audio_ion_data,
	struct msm_audio_alloc_data *alloc_data)
{
	/*
	 * Since these APIs can be invoked by multiple
	 * clients, there is need to make sure the list
	 * of allocations is always protected
	 */
	mutex_lock(&(msm_audio_ion_data->list_mutex));
	list_add_tail(&(alloc_data->list),
		      &(msm_audio_ion_data->alloc_list));
	mutex_unlock(&(msm_audio_ion_data->list_mutex));
}

static int msm_audio_dma_buf_map(void *handle, void *vaddr,
					  dma_addr_t *paddr,
					  size_t *len)
{
	struct msm_audio_alloc_data *alloc_data;

	/* Data required per buffer mapping */
	alloc_data = kzalloc(sizeof(*alloc_data), GFP_KERNEL);
	if (!alloc_data)
		return -ENOMEM;

	alloc_data->handle = handle;
	alloc_data->len = *len;
	alloc_data->vaddr = vaddr;
	alloc_data->paddr = paddr;
	alloc_data->type = MSM_AUDIO_MEM_TYPE_DMA;

	msm_audio_ion_add_allocation(&msm_audio_ion_data,
				     alloc_data);

	return 0;
}

static int msm_audio_ion_dma_buf_map(struct dma_buf *dma_buf,
				 dma_addr_t *addr, size_t *len, bool cma_mem)
{

	struct msm_audio_alloc_data *alloc_data = NULL;
	struct device *cb_dev;
	unsigned long ionflag = 0;
	int rc = 0;

	if (cma_mem)
		cb_dev = msm_audio_ion_data.cb_cma_dev;
	else
		cb_dev = msm_audio_ion_data.cb_dev;

	/* Data required per buffer mapping */
	alloc_data = kzalloc(sizeof(*alloc_data), GFP_KERNEL);
	if (!alloc_data)
		return -ENOMEM;

	alloc_data->handle = (void *)dma_buf;
	alloc_data->len = dma_buf->size;
	alloc_data->type = MSM_AUDIO_MEM_TYPE_ION;
	*len = dma_buf->size;

	/* Attach the dma_buf to context bank device */
	alloc_data->attach = dma_buf_attach(dma_buf, cb_dev);
	if (IS_ERR(alloc_data->attach)) {
		rc = PTR_ERR(alloc_data->attach);
		dev_err(cb_dev,
			"%s: Fail to attach dma_buf to CB, rc = %d\n",
			__func__, rc);
		goto free_alloc_data;
	}

	/* For uncached buffers, avoid cache maintanance */
	rc = dma_buf_get_flags(dma_buf, &ionflag);
	if (rc) {
		dev_err(cb_dev, "%s: dma_buf_get_flags failed: %d\n",
			__func__, rc);
		goto detach_dma_buf;
	}

	if (!(ionflag & ION_FLAG_CACHED))
		alloc_data->attach->dma_map_attrs |= DMA_ATTR_SKIP_CPU_SYNC;

	/*
	 * Get the scatter-gather list.
	 * There is no info as this is a write buffer or
	 * read buffer, hence the request is bi-directional
	 * to accommodate both read and write mappings.
	 */
	alloc_data->table = dma_buf_map_attachment(alloc_data->attach,
				DMA_BIDIRECTIONAL);
	if (IS_ERR(alloc_data->table)) {
		rc = PTR_ERR(alloc_data->table);
		dev_err(cb_dev,
			"%s: Fail to map attachment, rc = %d\n",
			__func__, rc);
		goto detach_dma_buf;
	}

	/* physical address from mapping */
	*addr = MSM_AUDIO_ION_PHYS_ADDR(alloc_data);
	alloc_data->paddr = addr;
	msm_audio_ion_add_allocation(&msm_audio_ion_data,
				     alloc_data);
	return rc;

detach_dma_buf:
	dma_buf_detach(dma_buf, alloc_data->attach);
free_alloc_data:
	kfree(alloc_data);
	alloc_data = NULL;

	return rc;
}

static int msm_audio_ion_unmap_kernel(void *vaddr, void *handle, bool cma_mem)
{
	int rc = 0;
	struct device *cb_dev;

	if (cma_mem)
		cb_dev = msm_audio_ion_data.cb_cma_dev;
	else
		cb_dev = msm_audio_ion_data.cb_dev;

	if (!vaddr) {
		dev_err(cb_dev,
			"%s: cannot find allocation for handle %pK\n",
			__func__, handle);
		rc = -EINVAL;
		goto err;
	}

	dma_buf_vunmap((struct dma_buf *)handle, vaddr);

	rc = dma_buf_end_cpu_access((struct dma_buf *)handle, DMA_BIDIRECTIONAL);
	if (rc) {
		dev_err(cb_dev, "%s: kmap dma_buf_end_cpu_access fail\n",
			__func__);
		goto err;
	}

err:
	return rc;
}

static int msm_audio_dma_buf_unmap(void *handle, bool cma_mem)
{
	int rc = 0;
	struct msm_audio_alloc_data *alloc_data = NULL;
	struct list_head *ptr, *next;
	struct device *cb_dev;
	bool found = false;

	if (cma_mem)
		cb_dev = msm_audio_ion_data.cb_cma_dev;
	else
		cb_dev = msm_audio_ion_data.cb_dev;
	/*
	 * Though list_for_each_safe is delete safe, lock
	 * should be explicitly acquired to avoid race condition
	 * on adding elements to the list.
	 */
	mutex_lock(&(msm_audio_ion_data.list_mutex));
	list_for_each_safe(ptr, next, &(msm_audio_ion_data.alloc_list)) {

		alloc_data = list_entry(ptr, struct msm_audio_alloc_data, list);
		if (alloc_data->type == MSM_AUDIO_MEM_TYPE_ION) {
			if (alloc_data->handle == handle) {
				/*
				 * The kernel mapping is in vmap (vaddr is
				 * never set for dma-bufs); imported CMA
				 * buffers have none.
				 */
				if (alloc_data->vmap)
					rc = msm_audio_ion_unmap_kernel(
							alloc_data->vmap,
							handle, cma_mem);
			if (rc) {
				pr_err("%s: Unable to unmap ion mem rc: %d\n",
				       __func__, rc);
				mutex_unlock(&(msm_audio_ion_data.list_mutex));
				return rc;
			}

				found = true;
				kfree(alloc_data->vmap);
				dma_buf_unmap_attachment(alloc_data->attach,
							 alloc_data->table,
							 DMA_BIDIRECTIONAL);

				dma_buf_detach((struct dma_buf *)
						alloc_data->handle,
					       alloc_data->attach);

				dma_buf_put((struct dma_buf *)
					    alloc_data->handle);

				list_del(&(alloc_data->list));
				kfree(alloc_data);
				break;
			}
		} else {
			alloc_data = list_entry(ptr,
						struct msm_audio_alloc_data,
						list);

			if (alloc_data->handle == handle) {
				found = true;

				dma_free_coherent(cb_dev, alloc_data->len,
						  alloc_data->vaddr,
						  *(alloc_data->paddr));

				list_del(&(alloc_data->list));
				kfree(alloc_data);
				break;
			}
		}
	}
	mutex_unlock(&(msm_audio_ion_data.list_mutex));

	if (!found) {
		dev_err(cb_dev,
			"%s: cannot find allocation, handle %pK\n",
			__func__, handle);
		rc = -EINVAL;
	}

	return rc;
}

static int msm_audio_ion_get_phys(struct dma_buf *dma_buf,
				  dma_addr_t *addr, size_t *len)
{
	int rc = 0;

	rc = msm_audio_ion_dma_buf_map(dma_buf, addr, len, false);
	if (rc) {
		pr_err("%s: failed to map DMA buf, err = %d\n",
			__func__, rc);
		goto err;
	}
	if (msm_audio_ion_data.smmu_enabled) {
		/* Append the SMMU SID information to the IOVA address */
		*addr |= msm_audio_ion_data.smmu_sid_bits;
	}

	pr_debug("phys=%pK, len=%zd, rc=%d\n", addr, *len, rc);
err:
	return rc;
}

int msm_audio_ion_get_smmu_info(struct device **cb_dev,
		u64 *smmu_sid)
{
	if (!cb_dev || !smmu_sid) {
		pr_err("%s: Invalid params\n",
			__func__);
		return -EINVAL;
	}

	if (!msm_audio_ion_data.cb_dev ||
		!msm_audio_ion_data.smmu_sid_bits) {
		pr_err("%s: Params not initialized\n",
			__func__);
		return -EINVAL;
	}

	*cb_dev = msm_audio_ion_data.cb_dev;
	*smmu_sid = msm_audio_ion_data.smmu_sid_bits;

	return 0;
}

static int msm_audio_ion_map_kernel(void *handle,
				struct iosys_map *iosys_vmap)
{
	int rc = 0;
	struct msm_audio_alloc_data *alloc_data = NULL;

	rc = dma_buf_begin_cpu_access((struct dma_buf *)handle,
				      DMA_BIDIRECTIONAL);
	if (rc) {
		pr_err("%s: kmap dma_buf_begin_cpu_access fail\n", __func__);
		goto exit;
	}

	rc = dma_buf_vmap((struct dma_buf *)handle, iosys_vmap);
	if (rc) {
		pr_err("%s: kernel mapping of dma_buf failed\n",
		       __func__);
		goto exit;
	}

	/*
	 * TBD: remove the below section once new API
	 * for mapping kernel virtual address is available.
	 */
	mutex_lock(&(msm_audio_ion_data.list_mutex));
	list_for_each_entry(alloc_data, &(msm_audio_ion_data.alloc_list),
			    list) {
		if (alloc_data->handle == handle) {
			alloc_data->vmap = iosys_vmap;
			break;
		}
	}
	mutex_unlock(&(msm_audio_ion_data.list_mutex));

exit:
	return rc;
}

static int msm_audio_protect_memory_region(struct platform_device *pdev)
{
	int ret = 0;
	phys_addr_t addr = 0;
	struct reserved_mem *rmem = NULL;
	u64 size = 0;
	struct device_node *node = NULL;
	u64 srcVM[1] = {VMID_HLOS};
	struct qcom_scm_vmperm destVM[2] = {{VMID_MSS_MSA, PERM_READ | PERM_WRITE},
		{VMID_HLOS, PERM_READ | PERM_WRITE}};
	node = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
	if (!node) {
		pr_err("node is NULL\n");
		ret = -EINVAL;
		goto exit;
	}
	rmem = of_reserved_mem_lookup(node);
	if (!rmem) {
		pr_err("unable to acquire memory-region of heap\n");
		ret = -EINVAL;
		goto exit;
	}
	ret = of_reserved_mem_device_init_by_idx(&pdev->dev, pdev->dev.of_node, 0);
	of_node_put(node);
	if (ret) {
		pr_err("Failed to initialize reserved mem, ret %d\n", ret);
		goto exit;
	}
	addr = rmem->base;
	size = (size_t)rmem->size;
	pr_err("%s: addr = %p size = %zu\n", __func__, (void *)(phys_addr_t)addr, (size_t)size);
	return qcom_scm_assign_mem(addr, size, srcVM, destVM, ARRAY_SIZE(destVM));
exit:
	return ret;
}

static int msm_audio_ion_map_buf(void *handle, dma_addr_t *paddr,
				 size_t *plen, struct iosys_map *iosys_vmap)
{
	int rc = 0;

	if (!handle || !paddr || !plen) {
		pr_err("%s: Invalid params\n", __func__);
		return -EINVAL;
	}

	rc = msm_audio_ion_get_phys((struct dma_buf *)handle, paddr, plen);
	if (rc) {
		pr_err("%s: ION Get Physical for AUDIO failed, rc = %d\n",
				__func__, rc);
		dma_buf_put((struct dma_buf *) handle);
		goto err;
	}

	rc = msm_audio_ion_map_kernel(handle, iosys_vmap);
	if (rc) {
		pr_err("%s: ION memory mapping for AUDIO failed, err:%d\n",
				__func__, rc);
		rc = -ENOMEM;
		msm_audio_dma_buf_unmap((struct dma_buf *)handle, false);
		goto err;
	}

err:
	return rc;
}

static u32 msm_audio_ion_get_smmu_sid_mode32(void)
{
	if (msm_audio_ion_data.smmu_enabled)
		return upper_32_bits(msm_audio_ion_data.smmu_sid_bits);
	else
		return 0;
}

/**
 * msm_audio_ion_alloc -
 *        Allocs ION memory for given client name
 *
 * @handle: generic handle to the memory allocation
 *          dma_buf for the system heap memory. vaddr for audio heap memory.
 * @bufsz: buffer size
 * @paddr: Physical address to be assigned with allocated region
 * @plen: length of allocated region to be assigned
 * vaddr: virtual address to be assigned
 *
 * Returns 0 on success or error on failure
 */
int msm_audio_ion_alloc(void **handle, size_t bufsz,
			dma_addr_t *paddr, size_t *plen, void **vaddr)
{
	int rc = -EINVAL;
	unsigned long err_ion_ptr = 0;
	struct dma_heap *heap = NULL;
	struct iosys_map *iosys_vmap = NULL;

	if (!(msm_audio_ion_data.device_status & MSM_AUDIO_ION_PROBED)) {
		pr_debug("%s:probe is not done, deferred\n", __func__);
		return -EPROBE_DEFER;
	}
	if (!handle || !paddr || !vaddr || !bufsz || !plen) {
		pr_err("%s: Invalid params\n", __func__);
		return -EINVAL;
	}

	iosys_vmap = kzalloc(sizeof(*iosys_vmap), GFP_KERNEL);
	if (!iosys_vmap) {
		rc = -ENOMEM;
		goto err;
	}

	if (msm_audio_ion_data.smmu_enabled == true) {
		pr_debug("%s: system heap is used\n", __func__);
		heap = dma_heap_find("qcom,system-uncached");
		if (!heap) {
			pr_err("Unable to find the system-uncached heap\n");
			kfree(iosys_vmap);
			goto err;
		}
		*handle = dma_heap_buffer_alloc(heap, bufsz, 0, 0);
	} else {
		pr_debug("%s: audio heap is used\n", __func__);
		*vaddr = *handle = dma_alloc_coherent(
						      msm_audio_ion_data.cb_dev,
						      bufsz, paddr, GFP_KERNEL);
		if (*vaddr != NULL) {
			pr_err("%s: vaddr = %pK, size=%zd\n", __func__, *vaddr,
			       bufsz);
			rc = 0;
		}
	}

	if (IS_ERR_OR_NULL((void *)(*handle))) {
		if (IS_ERR((void *)(*handle)))
			err_ion_ptr = PTR_ERR((void *)(*handle));
		pr_err("%s: ION alloc fail err ptr=%ld, smmu_enabled=%d\n",
		       __func__, err_ion_ptr, msm_audio_ion_data.smmu_enabled);
		rc = -ENOMEM;
		kfree(iosys_vmap);
		goto err;
	}

	if (msm_audio_ion_data.smmu_enabled) {
		rc = msm_audio_ion_map_buf(*handle, paddr, plen, iosys_vmap);
		if (rc) {
			pr_err("%s: failed to map ION buf, rc = %d\n", __func__,
			       rc);
			kfree(iosys_vmap);
			goto err;
		}
		*vaddr = iosys_vmap->vaddr;
	} else {
		rc = msm_audio_dma_buf_map(*handle, *vaddr, paddr,
						    &bufsz);
		if (rc) {
			pr_err("%s: failed to map ION buf, rc = %d\n", __func__,
				rc);
			dma_free_coherent(msm_audio_ion_data.cb_dev,
					  bufsz, vaddr, *paddr);
			goto err;
		}
	}

	pr_debug("%s: mapped address = %p, size=%zd\n", __func__,
		*vaddr, bufsz);

	memset(*vaddr, 0, bufsz);

err:
	return rc;
}
EXPORT_SYMBOL(msm_audio_ion_alloc);

/**
 * msm_audio_ion_dma_map -
 *        Memory maps for a given DMA buffer
 *
 * @phys_addr: Physical address of DMA buffer to be mapped
 * @iova_base: IOVA address of memory mapped DMA buffer
 * @size: buffer size
 * @dir: DMA direction
 * Returns 0 on success or error on failure
 */
int msm_audio_ion_dma_map(dma_addr_t *phys_addr, dma_addr_t *iova_base,
			u32 size, enum dma_data_direction dir)
{
	dma_addr_t iova;
	struct device *cb_dev = msm_audio_ion_data.cb_dev;

	if (!phys_addr || !iova_base || !size)
		return -EINVAL;

	iova = dma_map_resource(cb_dev, *phys_addr, size,
				dir, 0);
	if (dma_mapping_error(cb_dev, iova)) {
		pr_err("%s: dma_mapping_error\n", __func__);
		return -EIO;
	}
	pr_debug("%s: dma_mapping_success iova:0x%lx\n", __func__,
			 (unsigned long)iova);
	if (msm_audio_ion_data.smmu_enabled)
		/* Append the SMMU SID information to the IOVA address */
		iova |= msm_audio_ion_data.smmu_sid_bits;

	*iova_base = iova;

	return 0;
}
EXPORT_SYMBOL(msm_audio_ion_dma_map);

/**
 * msm_audio_ion_import-
 *        Import ION buffer with given file descriptor
 *
 * @handle: generic handle to the memory allocation.
 * dma_buf for the ION memory.
 * @fd: file descriptor for the ION memory
 * @ionflag: flags associated with ION buffer
 * @bufsz: buffer size
 * @paddr: Physical address to be assigned with allocated region
 * @plen: length of allocated region to be assigned
 * @iosys_vmap: Virtual mapping vmap pointer to be assigned
 *
 * Returns 0 on success or error on failure
 */
int msm_audio_ion_import(void **handle, int fd,
			unsigned long *ionflag, size_t bufsz,
			dma_addr_t *paddr, size_t *plen, void **vaddr)
{
	int rc = 0;
	struct iosys_map *iosys_vmap = NULL;

	if (!(msm_audio_ion_data.device_status & MSM_AUDIO_ION_PROBED)) {
		pr_debug("%s: probe is not done, deferred\n", __func__);
		return -EPROBE_DEFER;
	}

	if (!handle || !paddr || !vaddr || !plen) {
		pr_err("%s: Invalid params\n", __func__);
		return -EINVAL;
	}

	iosys_vmap = kzalloc(sizeof(*iosys_vmap), GFP_KERNEL);
	if (!iosys_vmap)
		return -ENOMEM;

	/* bufsz should be 0 and fd shouldn't be 0 as of now */
	*handle = dma_buf_get(fd);
	pr_debug("%s: handle =%pK, fd=%d\n", __func__, *handle, fd);
	if (IS_ERR_OR_NULL((void *)(*handle))) {
		pr_err("%s: dma_buf_get failed\n", __func__);
		rc = -EINVAL;
		goto err;
	}

	if (ionflag != NULL) {
		rc = dma_buf_get_flags((struct dma_buf *)*handle, ionflag);
		if (rc) {
			pr_err("%s: could not get flags for the dma_buf\n",
				__func__);
			goto err_ion_flag;
		}
	}

	rc = msm_audio_ion_map_buf(*handle, paddr, plen, iosys_vmap);
	if (rc) {
		pr_err("%s: failed to map ION buf, rc = %d\n", __func__, rc);
		goto err;
	}

	*vaddr = iosys_vmap->vaddr;
	pr_debug("%s: mapped address = %pK, size=%zd\n", __func__,
		*vaddr, bufsz);

	return 0;

err_ion_flag:
	dma_buf_put((struct dma_buf *) *handle);
err:
	kfree(iosys_vmap);
	*handle = NULL;
	return rc;
}
EXPORT_SYMBOL(msm_audio_ion_import);

/**
 * msm_audio_ion_import_cma-
 *        Import ION buffer with given file descriptor
 *
 * @handle: generic handle to the memory allocation
 * @fd: file descriptor for the ION memory
 * @ionflag: flags associated with ION buffer
 * @bufsz: buffer size
 * @paddr: Physical address to be assigned with allocated region
 * @plen: length of allocated region to be assigned
 * vaddr: virtual address to be assigned
 *
 * Returns 0 on success or error on failure
 */
int msm_audio_ion_import_cma(void  **handle, int fd,
			unsigned long *ionflag, size_t bufsz,
			dma_addr_t *paddr, size_t *plen, void **vaddr)
{
	int rc = 0;

	if (!(msm_audio_ion_data.device_status & MSM_AUDIO_ION_PROBED)) {
		pr_debug("%s: probe is not done, deferred\n", __func__);
		return -EPROBE_DEFER;
	}

	if (!handle || !paddr || !vaddr || !plen ||
	    !msm_audio_ion_data.cb_cma_dev) {
		pr_err("%s: Invalid params\n", __func__);
		return -EINVAL;
	}

	/* bufsz should be 0 and fd shouldn't be 0 as of now */
	*handle = dma_buf_get(fd);
	pr_debug("%s: dma_buf =%pK, fd=%d\n", __func__, *handle, fd);
	if (IS_ERR_OR_NULL((void *)(*handle))) {
		pr_err("%s: dma_buf_get failed\n", __func__);
		rc = -EINVAL;
		goto err;
	}

	if (ionflag != NULL) {
		rc = dma_buf_get_flags(*handle, ionflag);
		if (rc) {
			pr_err("%s: could not get flags for the dma_buf\n",
				__func__);
			goto err_ion_flag;
		}
	}

	/* As 5.4: attach and map it, it is freed as an ION buffer */
	rc = msm_audio_ion_dma_buf_map(*handle, paddr, plen, true);
	if (rc)
		goto err_ion_flag;

	return 0;

err_ion_flag:
	dma_buf_put((struct dma_buf *) *handle);
err:
	*handle = NULL;
	return rc;
}
EXPORT_SYMBOL(msm_audio_ion_import_cma);

/**
 * msm_audio_ion_free -
 *        fress ION memory for given client and handle
 *
 * @handle: generic handle to the memory allocation
 *          dma_buf for the system heap memory. vaddr for audio heap memory.
 *
 * Returns 0 on success or error on failure
 */
int msm_audio_ion_free(void *handle)
{
	int ret = 0;

	if (!handle) {
		pr_err("%s: handle invalid\n", __func__);
		return -EINVAL;
	}

	ret = msm_audio_dma_buf_unmap(handle, false);

	return ret;
}
EXPORT_SYMBOL(msm_audio_ion_free);

/**
 * msm_audio_ion_free_cma -
 *        fress ION memory for given client and handle
 *
 * @handle: generic handle to the memory allocation
 *
 * Returns 0 on success or error on failure
 */
int msm_audio_ion_free_cma(void *handle)
{
	if (!handle) {
		pr_err("%s: dma_buf invalid\n", __func__);
		return -EINVAL;
	}

	msm_audio_dma_buf_unmap(handle, true);

	return 0;
}
EXPORT_SYMBOL(msm_audio_ion_free_cma);

/**
 * msm_audio_ion_mmap -
 *       Audio ION memory map
 *
 * @abuff: audio buf pointer
 * @vma: virtual mem area
 *
 * Returns 0 on success or error on failure
 */
int msm_audio_ion_mmap(struct audio_buffer *abuff,
		       struct vm_area_struct *vma)
{
	struct msm_audio_alloc_data *alloc_data = NULL;
	struct sg_table *table;
	unsigned long addr = vma->vm_start;
	unsigned long offset = vma->vm_pgoff * PAGE_SIZE;
	struct scatterlist *sg;
	unsigned int i;
	struct page *page;
	int ret = 0;
	bool found = false;
	struct device *cb_dev = msm_audio_ion_data.cb_dev;

	mutex_lock(&(msm_audio_ion_data.list_mutex));
	list_for_each_entry(alloc_data, &(msm_audio_ion_data.alloc_list),
			    list) {
		if (alloc_data->handle == abuff->mem_handle) {
			found = true;
			table = alloc_data->table;
			break;
		}
	}
	mutex_unlock(&(msm_audio_ion_data.list_mutex));

	if (!found) {
		dev_err(cb_dev,
			"%s: cannot find allocation, dma_buf %pK",
			__func__, abuff->mem_handle);
		return -EINVAL;
	}
	/* uncached */
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	/* We need to check if a page is associated with this sg list because:
	 * If the allocation came from a carveout we currently don't have
	 * pages associated with carved out memory. This might change in the
	 * future and we can remove this check and the else statement.
	 */

	if (!table->sgl) {
		pr_err("%s: table->sgl is NULL\n", __func__);
		return -EINVAL;
	}

	page = sg_page(table->sgl);
	if (page) {
		pr_debug("%s: page is NOT null\n", __func__);
		for_each_sg(table->sgl, sg, table->orig_nents, i) {
			unsigned long remainder = vma->vm_end - addr;
			unsigned long len;

			if (!sg) {
				pr_err("%s: sg is NULL\n", __func__);
				return -EINVAL;
			}
			len = sg->length;

			page = sg_page(sg);

			if (offset >= len) {
				offset -= len;
				continue;
			} else if (offset) {
				page += offset / PAGE_SIZE;
				len -= offset;
				offset = 0;
			}
			len = min(len, remainder);
			pr_debug("vma=%pK, addr=%x len=%ld vm_start=%x vm_end=%x vm_page_prot=%lu\n",
				vma, (unsigned int)addr, len,
				(unsigned int)vma->vm_start,
				(unsigned int)vma->vm_end,
				(unsigned long)pgprot_val(vma->vm_page_prot));
			remap_pfn_range(vma, addr, page_to_pfn(page), len,
					vma->vm_page_prot);
			addr += len;
			if (addr >= vma->vm_end)
				return 0;
		}
	} else {
		pr_debug("%s: page is NULL\n", __func__);
		ret = -EINVAL;
	}

	return ret;
}
EXPORT_SYMBOL(msm_audio_ion_mmap);

/**
 * msm_audio_populate_upper_32_bits -
 *        retrieve upper 32bits of 64bit address
 *
 * @pa: 64bit physical address
 *
 */
u32 msm_audio_populate_upper_32_bits(dma_addr_t pa)
{
	if (sizeof(dma_addr_t) == sizeof(u32))
		return msm_audio_ion_get_smmu_sid_mode32();
	else
		return upper_32_bits(pa);
}
EXPORT_SYMBOL(msm_audio_populate_upper_32_bits);

static int msm_audio_smmu_init(struct device *dev)
{
	INIT_LIST_HEAD(&msm_audio_ion_data.alloc_list);
	mutex_init(&(msm_audio_ion_data.list_mutex));

	return 0;
}

static const struct of_device_id msm_audio_ion_dt_match[] = {
	{ .compatible = "qcom,msm-audio-ion" },
	{ .compatible = "qcom,msm-audio-ion-cma"},
	{ }
};
MODULE_DEVICE_TABLE(of, msm_audio_ion_dt_match);

static int msm_audio_ion_probe(struct platform_device *pdev)
{
	int rc = 0;
	u64 smmu_sid = 0;
	u64 smmu_sid_mask = 0;
	const char *msm_audio_ion_dt = "qcom,smmu-enabled";
	const char *msm_audio_ion_smmu = "qcom,smmu-version";
	const char *msm_audio_ion_smmu_sid_mask = "qcom,smmu-sid-mask";
	const char *mdm_audio_ion_scm = "qcom,scm-mp-enabled";
	bool smmu_enabled;
	bool scm_mp_enabled;
	enum apr_subsys_state q6_state;
	struct device *dev = &pdev->dev;
	struct of_phandle_args iommuspec;


	if (dev->of_node == NULL) {
		dev_err(dev,
			"%s: device tree is not found\n",
			__func__);
		msm_audio_ion_data.smmu_enabled = 0;
		return 0;
	}

	if (of_device_is_compatible(dev->of_node, "qcom,msm-audio-ion-cma")) {
		msm_audio_ion_data.cb_cma_dev = dev;
		return 0;
	}
	smmu_enabled = of_property_read_bool(dev->of_node,
					     msm_audio_ion_dt);
	msm_audio_ion_data.smmu_enabled = smmu_enabled;

	if (!smmu_enabled) {
		dev_dbg(dev, "%s: SMMU is Disabled\n", __func__);
				scm_mp_enabled = of_property_read_bool(dev->of_node,
						mdm_audio_ion_scm);
		if (scm_mp_enabled) {
			dev_dbg(dev, "%s: Calling HYP Assign\n", __func__);
			rc = msm_audio_protect_memory_region(pdev);
			if (rc) {
				dev_err(dev, "%s: HYP ASSIGN Failed\n", __func__);
				rc = 0;
			}
		} else
			pr_debug("%s: scm mp enabled not found\n", __func__);
		goto exit;
	}

	q6_state = apr_get_q6_state();
	if (q6_state == APR_SUBSYS_DOWN) {
		dev_dbg(dev,
			"defering %s, adsp_state %d\n",
			__func__, q6_state);
		return -EPROBE_DEFER;
	}
	dev_dbg(dev, "%s: adsp is ready\n", __func__);

	rc = of_property_read_u32(dev->of_node,
				msm_audio_ion_smmu,
				&msm_audio_ion_data.smmu_version);
	if (rc) {
		dev_err(dev,
			"%s: qcom,smmu_version missing in DT node\n",
			__func__);
		return rc;
	}
	dev_dbg(dev, "%s: SMMU is Enabled. SMMU version is (%d)",
		__func__, msm_audio_ion_data.smmu_version);

	/* Get SMMU SID information from Devicetree */
	rc = of_property_read_u64(dev->of_node,
				  msm_audio_ion_smmu_sid_mask,
				  &smmu_sid_mask);
	if (rc) {
		dev_err(dev,
			"%s: qcom,smmu-sid-mask missing in DT node, using default\n",
			__func__);
		smmu_sid_mask = 0xFFFFFFFFFFFFFFFF;
	}

	rc = of_parse_phandle_with_args(dev->of_node, "iommus",
					"#iommu-cells", 0, &iommuspec);
	if (rc)
		dev_err(dev, "%s: could not get smmu SID, ret = %d\n",
			__func__, rc);
	else
		smmu_sid = (iommuspec.args[0] & smmu_sid_mask);

	msm_audio_ion_data.smmu_sid_bits =
		smmu_sid << MSM_AUDIO_SMMU_SID_OFFSET;

	if (msm_audio_ion_data.smmu_version == 0x2) {
		rc = msm_audio_smmu_init(dev);
	} else {
		dev_err(dev, "%s: smmu version invalid %d\n",
			__func__, msm_audio_ion_data.smmu_version);
		rc = -EINVAL;
	}
	if (rc)
		dev_err(dev, "%s: smmu init failed, err = %d\n",
			__func__, rc);

exit:
	if (!rc)
		msm_audio_ion_data.device_status |= MSM_AUDIO_ION_PROBED;

	msm_audio_ion_data.cb_dev = dev;
	INIT_LIST_HEAD(&msm_audio_ion_data.alloc_list);
	mutex_init(&(msm_audio_ion_data.list_mutex));

	return rc;
}

static int msm_audio_ion_remove(struct platform_device *pdev)
{
	struct device *audio_cb_dev;

	audio_cb_dev = msm_audio_ion_data.cb_dev;

	msm_audio_ion_data.smmu_enabled = 0;
	msm_audio_ion_data.device_status = 0;
	mutex_destroy(&(msm_audio_ion_data.list_mutex));
	return 0;
}

static struct platform_driver msm_audio_ion_driver = {
	.driver = {
		.name = "msm-audio-ion",
		.owner = THIS_MODULE,
		.of_match_table = msm_audio_ion_dt_match,
		.suppress_bind_attrs = true,
	},
	.probe = msm_audio_ion_probe,
	.remove = msm_audio_ion_remove,
};

int __init msm_audio_ion_init(void)
{
	return platform_driver_register(&msm_audio_ion_driver);
}

void msm_audio_ion_exit(void)
{
	platform_driver_unregister(&msm_audio_ion_driver);
}

MODULE_DESCRIPTION("MSM Audio ION module");
MODULE_LICENSE("GPL v2");
