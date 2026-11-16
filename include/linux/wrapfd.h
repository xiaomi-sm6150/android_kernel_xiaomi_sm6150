/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * WrapFD kernel API
 *
 * Copyright (C) 2025 Google, Inc.
 */

#ifndef _LINUX_WRAPFD_H
#define _LINUX_WRAPFD_H

union wrapfd_mappable {
	struct dma_buf *dmabuf;
};

struct wrapfd_listener_operations {
	void (*on_content_release)(void *param);
};

#ifdef CONFIG_ANDROID_WRAPFD

/* Check if the vma is mapping a wrapfd file. */
bool is_wrapfd_vma(struct vm_area_struct *vma);

/*
 * Get mappable object. Caller also gets buffer ownership.
 * Function does not take file references, therefore the caller should
 * ensure the file is stable until wrapfd_put_mappable() is called.
 *
 * file: wrap file to get the mappable object.
 * dev: device requesting the operation.
 * mappable: mappable object.
 *
 * On success returns mappable object. On error returns:
 * -EBADF: file is invalid.
 * -ENODEV: device is invalid.
 * -EINVAL: buffer is mapped or mappable is NULL.
 * -EBUSY: buffer is owned by another device.
 * -ENOENT: wrap is empty (buffer got freed or moved)
 * -EOPNOTSUPP: wrapfd driver is not enabled
 */
int wrapfd_get_mappable(struct file *file, struct device *dev,
			union wrapfd_mappable *mappable);

/*
 * Release mappable object and buffer ownership. Caller should own the buffer.
 * Note that the caller might still keep the buffer mapped. If the buffer
 * is reused or freed then the next wrapfd_get_mappable() call will fail with
 * -ENOENT and the mapping should be invalidated. If wrapfd_get_mappable()
 * succeeds then previous mappings are still valid and can be reused.
 *
 * file: wrap file associated with the mappable object.
 * dev: device requesting the operation.
 * mappable: mappable object returned by wrapfd_get_mappable().
 *
 * On success returns 0. On error returns:
 * -EBADF: file is invalid.
 * -ENODEV: device is invalid.
 * -EOPNOTSUPP: wrapfd driver is not enabled
 * -EBUSY: buffer is not owned by the caller.
 * -EINVAL: wrap is empty or mappable is NULL or mappable's contents does not
 *	    match the one passed to wrapfd_get_mappable.
 */
int wrapfd_put_mappable(struct file *file, struct device *dev,
			union wrapfd_mappable *mappable);

/*
 * Register a listener for the mappable object currently wrapped by the @file
 * object. Caller should own the buffer.
 * The caller can keep the mappable object mapped in its device's address space
 * but it has to unmap it once on_content_release callback is called.
 *
 * on_content_release is invoked at most once, right before the content is
 * released (the wrap is emptied or rewrapped, or the last reference to the wrap
 * file is dropped). It is invoked in a sleepable context, in the context of the
 * task releasing the content, with no wrapfd locks held.
 *
 * @file is used only for the duration of this call. The caller does not have to
 * keep a reference to it for as long as the listener is registered, which lets
 * the content be released once userspace drops its last reference to the file.
 *
 * The returned handle stays valid until it is passed to wrapfd_del_listener(),
 * even after on_content_release was invoked or the file was released. Every
 * successful wrapfd_add_listener() call should be paired with exactly one
 * wrapfd_del_listener() call, which releases the resources of the listener.
 *
 * Note that on_content_release can be invoked as soon as the listener is added,
 * possibly before this function returns, unless the caller prevents the content
 * from being released, for example by holding a reference to @file.
 *
 * file: wrap file associated with the mappable object.
 * dev: device requesting the operation.
 * ops: wrapfd listener structure with event handlers. Should stay valid until
 *	wrapfd_del_listener() returns 0 or, if it is called from
 *	on_content_release of this listener, until that callback is done
 *	using them.
 * param: parameter passed to the event handler. Should stay valid for as long
 *	  as @ops.
 * handlep: pointer to variable receiving listener handle. Can't be NULL.
 *
 * On success returns 0. On error returns:
 * -EBADF: file is invalid.
 * -ENODEV: device is invalid.
 * -EOPNOTSUPP: wrapfd driver is not enabled
 * -EBUSY: buffer is not owned by the caller.
 * -ENOENT: wrap is empty (buffer got freed or moved).
 * -EINVAL: @ops or @handlep parameter is invalid.
 * -ENOMEM: not enough memory.
 */
int wrapfd_add_listener(struct file *file, struct device *dev,
			const struct wrapfd_listener_operations *ops,
			void *param, void **handlep);

/*
 * Unregister previously registered listener and free its handle. Caller does
 * not have to own the buffer or hold a reference to the wrap file, the file
 * might have been released already.
 * Should be called exactly once for each handle returned by
 * wrapfd_add_listener(), whether or not on_content_release was invoked. The
 * handle is invalid once the function returns 0.
 *
 * Once the function returns 0, on_content_release of the listener will never
 * be invoked anymore and, unless the function was called from that callback
 * itself, it is not running. The @ops and @param passed to
 * wrapfd_add_listener() can then be released; a callback which deleted its
 * own listener can release them once it is done using them. The function
 * does not tell whether the callback was invoked or not.
 *
 * If on_content_release of the listener is running or is about to be invoked
 * by another task, the function sleeps until the callback has returned.
 * Therefore it should be called from a sleepable context and the caller should
 * not hold any lock which on_content_release takes, otherwise it can deadlock.
 *
 * The function can be called from on_content_release, but only to delete the
 * listener this callback was invoked for. In this case it does not wait.
 * Deleting another listener of the same content which is still waiting for
 * its on_content_release fails with -EINVAL and leaves that listener and its
 * handle intact; it should be deleted later, outside of the callback.
 *
 * handle: listener handle returned by wrapfd_add_listener().
 *
 * On success returns 0. On error returns:
 * -EINVAL: handle is NULL or the function is called from on_content_release
 *	    of another listener, see above.
 * -EOPNOTSUPP: wrapfd driver is not enabled
 */
int wrapfd_del_listener(void *handle);

/*
 * Take file ownersip.
 * Function does not take file references, therefore the caller should
 * ensure the file is stable until wrapfd_put_ownership() is called.
 *
 * file: wrap file to get the mappable object.
 * dev: device requesting the operation.
 *
 * On success returns 0. On error returns:
 * -EBADF: file is invalid.
 * -ENODEV: device is invalid.
 * -EINVAL: buffer is mapped.
 * -EBUSY: buffer is owned by another device.
 * -ENOENT: wrap is empty (buffer got freed or moved)
 * -EOPNOTSUPP: wrapfd driver is not enabled
 */
int wrapfd_acquire_ownership(struct file *file, struct device *dev);

/*
 * Release buffer ownership. The caller should own the buffer.
 *
 * file: wrap file associated with the mappable object.
 * dev: device requesting the operation.
 *
 * -EBADF: file is invalid.
 * -ENODEV: device is invalid.
 * -EBUSY: buffer is not owned by the caller.
 * -EOPNOTSUPP: wrapfd driver is not enabled
 */
int wrapfd_release_ownership(struct file *file, struct device *dev);

#else /* CONFIG_ANDROID_WRAPFD */

static inline bool is_wrapfd_vma(struct vm_area_struct *vma)
{
	return false;
}

static inline int wrapfd_get_mappable(struct file *file, struct device *dev,
				      union wrapfd_mappable *mappable)
{
	return -EOPNOTSUPP;
}

static inline int wrapfd_put_mappable(struct file *file, struct device *dev,
				      union wrapfd_mappable *mappable)
{
	return -EOPNOTSUPP;
}

static inline int wrapfd_add_listener(struct file *file, struct device *dev,
				      const struct wrapfd_listener_operations *ops,
				      void *param, void **handlep);
{
	return -EOPNOTSUPP;
}

static inline int wrapfd_del_listener(void *handle)
{
	return -EOPNOTSUPP;
}

static inline int wrapfd_acquire_ownership(struct file *file, struct device *dev)
{
	return -EOPNOTSUPP;
}

static inline int wrapfd_release_ownership(struct file *file, struct device *dev)
{
	return -EOPNOTSUPP;
}

#endif /* CONFIG_ANDROID_WRAPFD */

#endif /* _LINUX_WRAPFD_H */
