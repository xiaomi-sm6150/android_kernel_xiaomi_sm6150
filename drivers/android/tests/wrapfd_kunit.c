// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit test for the wrapfd driver.
 *
 * Copyright (C) 2026, Google LLC.
 * Author: Suren Baghdasaryan <surenb@google.com>
 */
#include <kunit/device.h>
#include <kunit/test.h>

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/dma-buf.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/mman.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <linux/wrapfd.h>

struct test_buffer {
	struct page *page;
	size_t size;
	struct sg_table sgt;
	/* Counted in nr_tracked_buffers until released */
	bool tracked;
};

/* Lets a test wait until the buffers it created are released */
static atomic_t nr_tracked_buffers;
static DECLARE_WAIT_QUEUE_HEAD(tracked_buffers_wq);

static struct sg_table *test_map_dmabuf(struct dma_buf_attachment *attach,
		enum dma_data_direction direction)
{
	struct test_buffer *priv = attach->dmabuf->priv;

	return &priv->sgt;
}

static void test_unmap_dmabuf(struct dma_buf_attachment *attach,
		struct sg_table *sgt, enum dma_data_direction direction)
{
	/* No-op for dummy testing memory */
}

static void test_release(struct dma_buf *dmabuf)
{
	struct test_buffer *priv = dmabuf->priv;
	bool tracked = priv->tracked;

	sg_free_table(&priv->sgt);
	__free_page(priv->page);
	kfree(priv);
	if (tracked) {
		atomic_dec(&nr_tracked_buffers);
		wake_up(&tracked_buffers_wq);
	}
}

/* Minimal operations table required by dma_buf_export() */
static const struct dma_buf_ops test_dmabuf_ops = {
	.map_dma_buf   = test_map_dmabuf,
	.unmap_dma_buf = test_unmap_dmabuf,
	.release       = test_release,
};

static struct dma_buf *create_test_dmabuf(void)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct test_buffer *priv;
	struct dma_buf *dmabuf;
	int ret;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return ERR_PTR(-ENOMEM);

	priv->page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!priv->page) {
		kfree(priv);
		return ERR_PTR(-ENOMEM);
	}

	ret = sg_alloc_table_from_pages(&priv->sgt, &priv->page,
					1, 0, PAGE_SIZE, GFP_KERNEL);
	if (ret) {
		__free_page(priv->page);
		kfree(priv);
		return ERR_PTR(ret);
	}

	exp_info.ops = &test_dmabuf_ops;
	exp_info.size = PAGE_SIZE;
	exp_info.flags = O_RDWR;
	exp_info.priv = priv;

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		sg_free_table(&priv->sgt);
		__free_page(priv->page);
		kfree(priv);
	}

	return dmabuf;
}

static struct test_data {
	struct device *mock_dev1;
	struct device *mock_dev2;
	struct dma_buf *dmabuf;
	int empty_wrapfd;
	int wrapfd;
	bool emptied;
	struct file *file;
	struct file *empty_file;
} test_data;

static int wrapfd_suite_init(struct kunit_suite *suite)
{
	struct dma_buf *dmabuf;
	int empty_wrapfd;
	int dmabuf_fd;
	int wrapfd;
	int err;

	memset(&test_data, 0, sizeof(test_data));

	dmabuf = create_test_dmabuf();
	if (IS_ERR(dmabuf)) {
		err = PTR_ERR(dmabuf);
		pr_err("Failed to create DMA-BUF: %d\n", err);
		return err;
	}

	dmabuf_fd = dma_buf_fd(dmabuf, O_CLOEXEC);
	if (dmabuf_fd < 0) {
		err = dmabuf_fd;
		pr_err("Failed to install dmabuf fd: %d\n", err);
		dma_buf_put(dmabuf);
		return err;
	}

	/*
	 * Can't empty now because device to take the ownership is not ready yet.
	 * Will do that after devices are initialized in wrapfd_test_init().
	 */
	empty_wrapfd = create_wrapfd(dmabuf_fd, PROT_READ | PROT_WRITE);
	if (empty_wrapfd < 0) {
		err = empty_wrapfd;
		pr_err("Failed to create wrapfd: %d\n", err);
		goto close_dmabuf;
	}

	wrapfd = create_wrapfd(dmabuf_fd, PROT_READ | PROT_WRITE);
	if (wrapfd < 0) {
		err = wrapfd;
		pr_err("Failed to create wrapfd: %d\n", err);
		goto close_empty_wrapfd;
	}
	close_fd(dmabuf_fd);

	test_data.dmabuf = dmabuf;
	test_data.wrapfd = wrapfd;
	test_data.empty_wrapfd = empty_wrapfd;
	test_data.file = fget(wrapfd);
	test_data.empty_file = fget(empty_wrapfd);

	return 0;
close_empty_wrapfd:
	close_fd(empty_wrapfd);
close_dmabuf:
	close_fd(dmabuf_fd);

	return err;
}

static void wrapfd_suite_exit(struct kunit_suite *suite)
{
	if (!test_data.dmabuf)
		return;

	close_fd(test_data.empty_wrapfd);
	fput(test_data.file);

	close_fd(test_data.wrapfd);
	fput(test_data.empty_file);
}

static int wrapfd_test_init(struct kunit *test)
{
	struct device *dev1;
	struct device *dev2;

	if (!test_data.dmabuf)
		kunit_skip(test, "DMA-BUF was not created");

	dev1 = kunit_device_register(test, "mock_device1");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev1);
	test_data.mock_dev1 = dev1;

	dev2 = kunit_device_register(test, "mock_device2");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev2);
	test_data.mock_dev2 = dev2;

	if (!test_data.emptied) {
		KUNIT_ASSERT_EQ(test, wrapfd_acquire_ownership(test_data.empty_file, dev1), 0);
		KUNIT_ASSERT_EQ(test, empty_wrapfd(test_data.empty_file, dev1), 0);
		KUNIT_ASSERT_EQ(test, wrapfd_release_ownership(test_data.empty_file, dev1), 0);
		test_data.emptied = true;
	}

	return 0;
}

static void wrapfd_test_exit(struct kunit *test)
{
	if (test_data.mock_dev2) {
		kunit_device_unregister(test, test_data.mock_dev2);
		test_data.mock_dev2 = NULL;
	}
	if (test_data.mock_dev1) {
		kunit_device_unregister(test, test_data.mock_dev1);
		test_data.mock_dev1 = NULL;
	}
}

static void wrapfd_get_mappable_test(struct kunit *test)
{
	struct device *dev1 = test_data.mock_dev1;
	struct device *dev2 = test_data.mock_dev2;
	union wrapfd_mappable mappable;
	struct file *empty_file;
	struct file *file;

	empty_file = test_data.empty_file;
	file = test_data.file;

	/* Test invalid inputs */
	KUNIT_ASSERT_EQ(test, wrapfd_get_mappable(NULL, dev1, &mappable), -EBADF);
	KUNIT_ASSERT_EQ(test, wrapfd_put_mappable(NULL, dev1, &mappable), -EBADF);
	KUNIT_ASSERT_EQ(test, wrapfd_get_mappable(file, NULL, &mappable), -ENODEV);
	KUNIT_ASSERT_EQ(test, wrapfd_put_mappable(file, NULL, &mappable), -ENODEV);
	KUNIT_ASSERT_EQ(test, wrapfd_get_mappable(file, dev1, NULL), -EINVAL);
	KUNIT_ASSERT_EQ(test, wrapfd_put_mappable(file, dev1, NULL), -EINVAL);

	/* Get mappable object when no owners and no other users */
	KUNIT_ASSERT_EQ(test, wrapfd_get_mappable(file, dev1, &mappable), 0);
	KUNIT_ASSERT_EQ(test, wrapfd_put_mappable(file, dev1, &mappable), 0);

	/* Get mappable object with other users */
	KUNIT_ASSERT_EQ(test, wrapfd_get_mappable(file, dev1, &mappable), 0);
	KUNIT_ASSERT_EQ(test, wrapfd_get_mappable(file, dev2, &mappable), 0);
	KUNIT_ASSERT_EQ(test, wrapfd_put_mappable(file, dev1, &mappable), 0);
	KUNIT_ASSERT_EQ(test, wrapfd_put_mappable(file, dev2, &mappable), 0);

	/* Get mappable object while being the owner */
	KUNIT_ASSERT_EQ(test, wrapfd_acquire_ownership(file, dev1), 0);
	KUNIT_ASSERT_EQ(test, wrapfd_get_mappable(file, dev1, &mappable), 0);
	KUNIT_ASSERT_EQ(test, wrapfd_put_mappable(file, dev1, &mappable), 0);
	KUNIT_ASSERT_EQ(test, wrapfd_release_ownership(file, dev1), 0);

	/* Get mappable object while someone else is the owner */
	KUNIT_ASSERT_EQ(test, wrapfd_acquire_ownership(file, dev2), 0);
	KUNIT_ASSERT_EQ(test, wrapfd_get_mappable(file, dev1, &mappable), -EBUSY);
	KUNIT_ASSERT_EQ(test, wrapfd_release_ownership(file, dev2), 0);

	/* Get mappable object of an empty buffer */
	KUNIT_ASSERT_EQ(test, wrapfd_get_mappable(empty_file, dev1, &mappable),
					-ENOENT);
}

static void wrapfd_acquire_ownership_test(struct kunit *test)
{
	struct device *dev1 = test_data.mock_dev1;
	struct device *dev2 = test_data.mock_dev2;
	union wrapfd_mappable mappable;
	struct file *empty_file;
	struct file *file;

	empty_file = test_data.empty_file;
	file = test_data.file;

	/* Test invalid inputs */
	KUNIT_ASSERT_EQ(test, wrapfd_acquire_ownership(NULL, dev1), -EBADF);
	KUNIT_ASSERT_EQ(test, wrapfd_release_ownership(NULL, dev1), -EBADF);
	KUNIT_ASSERT_EQ(test, wrapfd_acquire_ownership(file, NULL), -ENODEV);
	KUNIT_ASSERT_EQ(test, wrapfd_release_ownership(file, NULL), -ENODEV);

	/* Get ownership when no owners and no other users */
	KUNIT_ASSERT_EQ(test, wrapfd_acquire_ownership(file, dev1), 0);
	KUNIT_ASSERT_EQ(test, wrapfd_release_ownership(file, dev1), 0);

	/* Get ownership while owning the buffer */
	KUNIT_ASSERT_EQ(test, wrapfd_acquire_ownership(file, dev1), 0);
	KUNIT_ASSERT_EQ(test, wrapfd_acquire_ownership(file, dev1), 0);
	KUNIT_ASSERT_EQ(test, wrapfd_release_ownership(file, dev1), 0);

	/* Get ownership while someone else owns the buffer */
	KUNIT_ASSERT_EQ(test, wrapfd_acquire_ownership(file, dev2), 0);
	KUNIT_ASSERT_EQ(test, wrapfd_acquire_ownership(file, dev1), -EBUSY);
	KUNIT_ASSERT_EQ(test, wrapfd_release_ownership(file, dev2), 0);

	/* Get ownership while mapping the buffer */
	KUNIT_ASSERT_EQ(test, wrapfd_get_mappable(file, dev1, &mappable), 0);
	KUNIT_ASSERT_EQ(test, wrapfd_acquire_ownership(file, dev1), -EINVAL);
	KUNIT_ASSERT_EQ(test, wrapfd_put_mappable(file, dev1, &mappable), 0);

	/* Get ownership while someone else maps the buffer */
	KUNIT_ASSERT_EQ(test, wrapfd_get_mappable(file, dev2, &mappable), 0);
	KUNIT_ASSERT_EQ(test, wrapfd_acquire_ownership(file, dev1), -EINVAL);
	KUNIT_ASSERT_EQ(test, wrapfd_put_mappable(file, dev2, &mappable), 0);

	/* Put ownership when nobody owns the buffer */
	KUNIT_ASSERT_EQ(test, wrapfd_release_ownership(file, dev1), -EBUSY);

	/* Put ownership when someone else owns the buffer */
	KUNIT_ASSERT_EQ(test, wrapfd_acquire_ownership(file, dev2), 0);
	KUNIT_ASSERT_EQ(test, wrapfd_release_ownership(file, dev1), -EBUSY);
	KUNIT_ASSERT_EQ(test, wrapfd_release_ownership(file, dev2), 0);

	/* Get ownership of an empty buffer */
	KUNIT_ASSERT_EQ(test, wrapfd_acquire_ownership(empty_file, dev2), -ENOENT);
}

/* How long to wait for events which happen in other threads */
#define TEST_TIMEOUT_MS	5000
/* How long to wait for events which should not happen */
#define TEST_NO_EVENT_MS	100

struct test_listener {
	/* NULL unless the listener is registered */
	void *handle;
	int nr_calls;
	/* Set right before the callback returns */
	bool finished;
	/* Delete the listener from inside its callback */
	bool self_delete;
	int self_delete_ret;
	/* Block the callback until allowed to proceed */
	bool block;
	struct completion started;
	struct completion proceed;
};

static int del_test_listener(struct test_listener *listener)
{
	int ret;

	ret = wrapfd_del_listener(listener->handle);
	/* The handle is invalid once the listener is deleted */
	if (!ret)
		listener->handle = NULL;

	return ret;
}

/*
 * Listeners live on the stack of the test, so none of them should stay
 * registered once the test is done, even if the test fails. For that reason
 * tests use expectations instead of assertions while they have listeners and
 * delete the ones that are still registered before returning.
 */
static void cleanup_test_listener(struct test_listener *listener)
{
	if (listener->handle)
		del_test_listener(listener);
}

static void test_on_content_release(void *param)
{
	struct test_listener *listener = param;

	listener->nr_calls++;
	if (listener->block) {
		complete(&listener->started);
		wait_for_completion(&listener->proceed);
	}
	if (listener->self_delete)
		listener->self_delete_ret = del_test_listener(listener);
	WRITE_ONCE(listener->finished, true);
}

static const struct wrapfd_listener_operations test_listener_ops = {
	.on_content_release = test_on_content_release,
};

static int add_test_listener(struct file *file, struct device *dev,
			     struct test_listener *listener)
{
	/* Drop the registration left by a call which should have failed */
	cleanup_test_listener(listener);

	return wrapfd_add_listener(file, dev, &test_listener_ops, listener,
				   &listener->handle);
}

/* Creates a wrap owned by @dev. Returns its file with a reference taken. */
static struct file *create_owned_wrap(struct kunit *test, struct device *dev, int *wrapfdp)
{
	struct test_buffer *buffer;
	struct dma_buf *dmabuf;
	struct file *file;
	int dmabuf_fd;
	int wrapfd;

	dmabuf = create_test_dmabuf();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dmabuf);
	dmabuf_fd = dma_buf_fd(dmabuf, O_CLOEXEC);
	KUNIT_ASSERT_GE(test, dmabuf_fd, 0);
	wrapfd = create_wrapfd(dmabuf_fd, PROT_READ | PROT_WRITE);
	close_fd(dmabuf_fd);
	KUNIT_ASSERT_GE(test, wrapfd, 0);
	file = fget(wrapfd);
	KUNIT_ASSERT_NOT_NULL(test, file);
	KUNIT_ASSERT_EQ(test, wrapfd_acquire_ownership(file, dev), 0);
	*wrapfdp = wrapfd;

	/* The wrap holds the only reference to the buffer, see release_owned_wrap() */
	buffer = dmabuf->priv;
	buffer->tracked = true;
	atomic_inc(&nr_tracked_buffers);

	return file;
}

/*
 * Waits until the buffers wrapped by create_owned_wrap() are released. The
 * buffer is released only after the wrap is done with its content, which
 * includes notifying the listeners, so once the buffer is gone no listener
 * callback is pending or running. Releasing the buffer is deferred to a work
 * when its last reference is dropped by a kthread, hence the wait.
 */
static void wait_for_buffers_release(struct kunit *test)
{
	KUNIT_EXPECT_NE(test,
			wait_event_timeout(tracked_buffers_wq,
					   !atomic_read(&nr_tracked_buffers),
					   msecs_to_jiffies(TEST_TIMEOUT_MS)), 0);
}

/*
 * Drops the references taken by create_owned_wrap() and waits until the wrapped
 * buffer is released. Tests run in a kthread, for which fput() defers releasing
 * the file to a work, so a wrap which was not emptied might still hold its
 * content when fput() returns.
 */
static void release_owned_wrap(struct kunit *test, struct file *file, int wrapfd)
{
	close_fd(wrapfd);
	fput(file);
	wait_for_buffers_release(test);
}

static void wrapfd_listener_test(struct kunit *test)
{
	struct device *dev1 = test_data.mock_dev1;
	struct device *dev2 = test_data.mock_dev2;
	struct test_listener l1 = {};
	struct test_listener l2 = { .self_delete = true, .self_delete_ret = 1 };
	/* Deleted before the content is released */
	struct test_listener deleted = {};
	/* Used by the calls which are expected to fail */
	struct test_listener failed = {};
	struct file *file;
	int wrapfd;

	kunit_skip(test,
		   "b/590561996: create_owned_wrap() cannot be called from kernel security domain");

	/* Test invalid inputs */
	KUNIT_EXPECT_EQ(test, wrapfd_del_listener(NULL), -EINVAL);
	KUNIT_EXPECT_EQ(test, add_test_listener(NULL, dev1, &failed), -EBADF);
	KUNIT_EXPECT_EQ(test, wrapfd_add_listener(test_data.file, dev1, NULL, &failed,
						  &failed.handle), -EINVAL);
	KUNIT_EXPECT_EQ(test, wrapfd_add_listener(test_data.file, dev1, &test_listener_ops,
						  &failed, NULL), -EINVAL);
	cleanup_test_listener(&failed);

	/* Add and delete a listener without owning the buffer */
	KUNIT_EXPECT_EQ(test, add_test_listener(test_data.file, dev1, &l1), 0);
	KUNIT_EXPECT_EQ(test, del_test_listener(&l1), 0);

	/* Delete a listener after someone else took the ownership of the buffer */
	KUNIT_EXPECT_EQ(test, add_test_listener(test_data.file, dev1, &l1), 0);
	KUNIT_EXPECT_EQ(test, wrapfd_acquire_ownership(test_data.file, dev2), 0);
	KUNIT_EXPECT_EQ(test, del_test_listener(&l1), 0);
	KUNIT_EXPECT_EQ(test, wrapfd_release_ownership(test_data.file, dev2), 0);

	/* No listeners are registered at this point, assertions are safe */
	file = create_owned_wrap(test, dev1, &wrapfd);

	/* Add a listener while someone else owns the buffer */
	KUNIT_EXPECT_EQ(test, add_test_listener(file, dev2, &failed), -EBUSY);

	/* A listener deleted before the content is released is never notified */
	KUNIT_EXPECT_EQ(test, add_test_listener(file, dev1, &deleted), 0);
	KUNIT_EXPECT_EQ(test, del_test_listener(&deleted), 0);

	/* Listeners are notified once and their handles stay valid afterwards */
	KUNIT_EXPECT_EQ(test, add_test_listener(file, dev1, &l1), 0);
	KUNIT_EXPECT_EQ(test, add_test_listener(file, dev1, &l2), 0);
	KUNIT_EXPECT_EQ(test, empty_wrapfd(file, dev1), 0);
	KUNIT_EXPECT_EQ(test, l1.nr_calls, 1);
	KUNIT_EXPECT_EQ(test, l2.nr_calls, 1);
	KUNIT_EXPECT_EQ(test, deleted.nr_calls, 0);
	/* A listener can be deleted from its own callback */
	KUNIT_EXPECT_EQ(test, l2.self_delete_ret, 0);
	/* Add a listener to an empty wrap */
	KUNIT_EXPECT_EQ(test, add_test_listener(file, dev1, &failed), -ENOENT);
	/* A notified listener which is not deleted yet does not pin the buffer */
	wait_for_buffers_release(test);
	/* Deleting a listener after its callback does not tell it was notified */
	KUNIT_EXPECT_EQ(test, del_test_listener(&l1), 0);
	KUNIT_EXPECT_EQ(test, l1.nr_calls, 1);

	cleanup_test_listener(&l1);
	cleanup_test_listener(&l2);
	cleanup_test_listener(&failed);
	release_owned_wrap(test, file, wrapfd);
}

/* The listener does not need the wrap file to stay around. */
static void wrapfd_listener_file_release_test(struct kunit *test)
{
	struct device *dev1 = test_data.mock_dev1;
	struct test_listener l1 = {}, l2 = {};
	struct file *file;
	int wrapfd;

	kunit_skip(test,
		   "b/590561996: create_owned_wrap() cannot be called from kernel security domain");

	file = create_owned_wrap(test, dev1, &wrapfd);
	KUNIT_EXPECT_EQ(test, add_test_listener(file, dev1, &l1), 0);
	KUNIT_EXPECT_EQ(test, add_test_listener(file, dev1, &l2), 0);
	/* Delete one listener before the file is released and one after */
	KUNIT_EXPECT_EQ(test, del_test_listener(&l2), 0);

	/* Drop all file references. The listener should get notified. */
	release_owned_wrap(test, file, wrapfd);
	KUNIT_EXPECT_EQ(test, l1.nr_calls, 1);
	KUNIT_EXPECT_EQ(test, l2.nr_calls, 0);

	/* The handle is still valid after the file is gone */
	KUNIT_EXPECT_EQ(test, del_test_listener(&l1), 0);
	KUNIT_EXPECT_EQ(test, l1.nr_calls, 1);
	cleanup_test_listener(&l1);
}

/* A listener registered by a non-owner is notified like any other. */
static void wrapfd_listener_non_owner_test(struct kunit *test)
{
	struct device *dev1 = test_data.mock_dev1;
	struct device *dev2 = test_data.mock_dev2;
	struct test_listener l1 = {};
	struct file *file;
	int wrapfd;

	kunit_skip(test,
		   "b/590561996: create_owned_wrap() cannot be called from kernel security domain");

	file = create_owned_wrap(test, dev1, &wrapfd);
	KUNIT_EXPECT_EQ(test, wrapfd_release_ownership(file, dev1), 0);

	/* Register while nobody owns the buffer, then let someone else own it */
	KUNIT_EXPECT_EQ(test, add_test_listener(file, dev2, &l1), 0);
	KUNIT_EXPECT_EQ(test, wrapfd_acquire_ownership(file, dev1), 0);

	/* The owner releases the content */
	KUNIT_EXPECT_EQ(test, empty_wrapfd(file, dev1), 0);
	KUNIT_EXPECT_EQ(test, l1.nr_calls, 1);
	KUNIT_EXPECT_EQ(test, del_test_listener(&l1), 0);

	cleanup_test_listener(&l1);
	release_owned_wrap(test, file, wrapfd);
}

struct test_empty_work {
	struct file *file;
	struct device *dev;
	int ret;
	struct completion done;
};

static int test_empty_thread(void *data)
{
	struct test_empty_work *work = data;

	work->ret = empty_wrapfd(work->file, work->dev);
	complete(&work->done);

	return 0;
}

struct test_del_work {
	struct test_listener *listener;
	int ret;
	/* Whether the callback had returned by the time the listener got deleted */
	bool callback_finished;
	/* The thread was started and has to be waited for */
	bool started;
	struct completion done;
};

static int test_del_thread(void *data)
{
	struct test_del_work *work = data;

	work->ret = del_test_listener(work->listener);
	work->callback_finished = READ_ONCE(work->listener->finished);
	complete(&work->done);

	return 0;
}

static void start_test_del_thread(struct kunit *test, struct test_del_work *work)
{
	struct task_struct *task;

	init_completion(&work->done);
	task = kthread_run(test_del_thread, work, "wrapfd_del");
	if (IS_ERR(task)) {
		KUNIT_FAIL(test, "failed to start a thread: %ld", PTR_ERR(task));
		return;
	}
	work->started = true;
}

/*
 * The thread uses the stack of the test, so it is waited for even if the test
 * has failed.
 */
static void finish_test_del_thread(struct kunit *test, struct test_del_work *work)
{
	if (!work->started)
		return;

	wait_for_completion(&work->done);
	KUNIT_EXPECT_EQ(test, work->ret, 0);
	/* The callback is not running once the listener is deleted */
	KUNIT_EXPECT_TRUE(test, work->callback_finished);
}

/*
 * wrapfd_del_listener() called for a listener picked for notification by
 * another task should wait until the callback of the listener has returned,
 * whether the callback is running or is still waiting to be invoked.
 */
static void wrapfd_listener_del_in_progress_test(struct kunit *test)
{
	struct device *dev1 = test_data.mock_dev1;
	struct test_listener running = { .block = true };
	struct test_listener queued = {};
	struct test_empty_work empty_work = { .dev = dev1 };
	struct test_del_work del_running = { .listener = &running };
	struct test_del_work del_queued = { .listener = &queued };
	struct task_struct *task;
	struct file *file;
	int wrapfd;

	kunit_skip(test,
		   "b/590561996: create_owned_wrap() cannot be called from kernel security domain");

	init_completion(&running.started);
	init_completion(&running.proceed);
	init_completion(&empty_work.done);

	file = create_owned_wrap(test, dev1, &wrapfd);
	empty_work.file = file;
	/*
	 * Listeners are notified starting from the one added last, so the
	 * blocked callback keeps the other listener waiting for its turn.
	 */
	KUNIT_EXPECT_EQ(test, add_test_listener(file, dev1, &queued), 0);
	KUNIT_EXPECT_EQ(test, add_test_listener(file, dev1, &running), 0);

	/* Release the content and wait until the callback is running */
	task = kthread_run(test_empty_thread, &empty_work, "wrapfd_empty");
	if (IS_ERR(task)) {
		KUNIT_FAIL(test, "failed to start a thread: %ld", PTR_ERR(task));
		goto cleanup;
	}

	if (wait_for_completion_timeout(&running.started,
					msecs_to_jiffies(TEST_TIMEOUT_MS))) {
		/*
		 * Delete the listener whose callback is still running and the
		 * one which was picked but not invoked yet. Neither deletion
		 * should finish while the callback is blocked.
		 */
		start_test_del_thread(test, &del_running);
		start_test_del_thread(test, &del_queued);
		if (del_running.started)
			KUNIT_EXPECT_EQ(test,
				wait_for_completion_timeout(&del_running.done,
					msecs_to_jiffies(TEST_NO_EVENT_MS)), 0);
		if (del_queued.started)
			KUNIT_EXPECT_FALSE(test, completion_done(&del_queued.done));
		KUNIT_EXPECT_EQ(test, queued.nr_calls, 0);
		KUNIT_EXPECT_FALSE(test, completion_done(&empty_work.done));
	} else {
		KUNIT_FAIL(test, "on_content_release was not invoked");
	}

	/*
	 * Let the callback finish. The threads use the stack of the test, so
	 * wait for them even if the test has failed. Nothing blocks them once
	 * the callback is allowed to proceed.
	 */
	complete(&running.proceed);
	wait_for_completion(&empty_work.done);
	finish_test_del_thread(test, &del_running);
	finish_test_del_thread(test, &del_queued);
	KUNIT_EXPECT_EQ(test, empty_work.ret, 0);
	/* Both listeners were picked for notification before being deleted */
	KUNIT_EXPECT_EQ(test, running.nr_calls, 1);
	KUNIT_EXPECT_EQ(test, queued.nr_calls, 1);
cleanup:
	cleanup_test_listener(&running);
	cleanup_test_listener(&queued);
	release_owned_wrap(test, file, wrapfd);
}

static void is_wrapfd_file_test(struct kunit *test)
{
	KUNIT_ASSERT_FALSE(test, is_wrapfd_file(NULL));
	KUNIT_ASSERT_FALSE(test, is_wrapfd_file(test_data.dmabuf->file));
	KUNIT_ASSERT_TRUE(test, is_wrapfd_file(test_data.file));
	KUNIT_ASSERT_TRUE(test, is_wrapfd_file(test_data.empty_file));
}

/*
 * KUnit tests run in a kthread, so once a mock device owns @file, current is
 * not the owner and wrap_llseek() treats it as a guest: seeking succeeds with
 * the buffer size iff guests are allowed, and fails with -EBUSY otherwise.
 */
static loff_t guest_llseek_end(struct file *file)
{
	return file->f_op->llseek(file, 0, SEEK_END);
}

static void wrapfd_allow_guests_test(struct kunit *test)
{
	struct file *dmabuf_file = test_data.dmabuf->file;
	struct file *empty_file = test_data.empty_file;
	struct device *dev1 = test_data.mock_dev1;
	struct device *dev2 = test_data.mock_dev2;
	struct file *file = test_data.file;
	union wrapfd_mappable mappable;
	struct file *owned_file;
	int wrapfd;

	kunit_skip(test,
		   "b/590561996: create_owned_wrap() cannot be called from kernel security domain");

	/* Test invalid inputs */
	KUNIT_ASSERT_EQ(test, wrapfd_allow_guests(NULL, dev1), -EBADF);
	KUNIT_ASSERT_EQ(test, wrapfd_prohibit_guests(NULL, dev1), -EBADF);
	KUNIT_ASSERT_EQ(test, wrapfd_allow_guests(dmabuf_file, dev1), -EBADF);
	KUNIT_ASSERT_EQ(test, wrapfd_prohibit_guests(dmabuf_file, dev1), -EBADF);
	KUNIT_ASSERT_EQ(test, wrapfd_allow_guests(file, NULL), -ENODEV);
	KUNIT_ASSERT_EQ(test, wrapfd_prohibit_guests(file, NULL), -ENODEV);

	/* Changing guest policy on unowned buffers fails with -EBUSY */
	KUNIT_ASSERT_EQ(test, wrapfd_allow_guests(file, dev1), -EBUSY);
	KUNIT_ASSERT_EQ(test, wrapfd_prohibit_guests(file, dev1), -EBUSY);
	KUNIT_ASSERT_EQ(test, wrapfd_allow_guests(empty_file, dev1), -EBUSY);
	KUNIT_ASSERT_EQ(test, wrapfd_prohibit_guests(empty_file, dev1), -EBUSY);

	/*
	 * Unowned buffer is accessible; acquiring ownership prohibits guests
	 * by default.
	 */
	KUNIT_ASSERT_EQ(test, guest_llseek_end(file), PAGE_SIZE);
	KUNIT_ASSERT_EQ(test, wrapfd_acquire_ownership(file, dev1), 0);
	KUNIT_EXPECT_EQ(test, guest_llseek_end(file), -EBUSY);

	/* Prohibiting when already prohibited is a no-op */
	KUNIT_EXPECT_EQ(test, wrapfd_prohibit_guests(file, dev1), 0);
	KUNIT_EXPECT_EQ(test, guest_llseek_end(file), -EBUSY);

	/* Allowing guests enables access and is idempotent */
	KUNIT_EXPECT_EQ(test, wrapfd_allow_guests(file, dev1), 0);
	KUNIT_EXPECT_EQ(test, guest_llseek_end(file), PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, wrapfd_allow_guests(file, dev1), 0);
	KUNIT_EXPECT_EQ(test, guest_llseek_end(file), PAGE_SIZE);

	/* Non-owner device cannot change guest policy */
	KUNIT_EXPECT_EQ(test, wrapfd_allow_guests(file, dev2), -EBUSY);
	KUNIT_EXPECT_EQ(test, wrapfd_prohibit_guests(file, dev2), -EBUSY);
	KUNIT_EXPECT_EQ(test, guest_llseek_end(file), PAGE_SIZE);

	/* Prohibiting guests revokes access and is idempotent */
	KUNIT_EXPECT_EQ(test, wrapfd_prohibit_guests(file, dev1), 0);
	KUNIT_EXPECT_EQ(test, guest_llseek_end(file), -EBUSY);
	KUNIT_EXPECT_EQ(test, wrapfd_prohibit_guests(file, dev1), 0);
	KUNIT_EXPECT_EQ(test, wrapfd_allow_guests(file, dev2), -EBUSY);
	KUNIT_EXPECT_EQ(test, guest_llseek_end(file), -EBUSY);

	/*
	 * Allowing guests while mapped succeeds; prohibiting while mapped
	 * fails with -EINVAL.
	 */
	KUNIT_EXPECT_EQ(test, wrapfd_get_mappable(file, dev1, &mappable), 0);
	KUNIT_EXPECT_EQ(test, wrapfd_prohibit_guests(file, dev1), -EINVAL);
	KUNIT_EXPECT_EQ(test, wrapfd_allow_guests(file, dev1), 0);
	KUNIT_EXPECT_EQ(test, guest_llseek_end(file), PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, wrapfd_prohibit_guests(file, dev1), -EINVAL);
	KUNIT_EXPECT_EQ(test, guest_llseek_end(file), PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, wrapfd_put_mappable(file, dev1, &mappable), 0);
	KUNIT_EXPECT_EQ(test, wrapfd_prohibit_guests(file, dev1), 0);
	KUNIT_EXPECT_EQ(test, guest_llseek_end(file), -EBUSY);

	/* Releasing ownership resets guest policy for the next owner */
	KUNIT_EXPECT_EQ(test, wrapfd_allow_guests(file, dev1), 0);
	KUNIT_EXPECT_EQ(test, guest_llseek_end(file), PAGE_SIZE);
	KUNIT_ASSERT_EQ(test, wrapfd_release_ownership(file, dev1), 0);
	KUNIT_ASSERT_EQ(test, wrapfd_acquire_ownership(file, dev2), 0);
	KUNIT_EXPECT_EQ(test, guest_llseek_end(file), -EBUSY);
	KUNIT_ASSERT_EQ(test, wrapfd_release_ownership(file, dev2), 0);

	/*
	 * Emptying an owned wrap rejects allow_guests (-ENOENT) while still
	 * allowing prohibit_guests.
	 */
	owned_file = create_owned_wrap(test, dev1, &wrapfd);
	KUNIT_EXPECT_EQ(test, wrapfd_allow_guests(owned_file, dev1), 0);
	KUNIT_EXPECT_EQ(test, empty_wrapfd(owned_file, dev1), 0);
	KUNIT_EXPECT_EQ(test, wrapfd_allow_guests(owned_file, dev1), -ENOENT);
	KUNIT_EXPECT_EQ(test, wrapfd_allow_guests(owned_file, dev2), -EBUSY);
	KUNIT_EXPECT_EQ(test, wrapfd_prohibit_guests(owned_file, dev2), -EBUSY);
	KUNIT_EXPECT_EQ(test, wrapfd_prohibit_guests(owned_file, dev1), 0);
	KUNIT_EXPECT_EQ(test, wrapfd_release_ownership(owned_file, dev1), 0);
	release_owned_wrap(test, owned_file, wrapfd);
}

static struct kunit_case wrapfd_test_cases[] = {
	KUNIT_CASE(wrapfd_get_mappable_test),
	KUNIT_CASE(wrapfd_acquire_ownership_test),
	KUNIT_CASE(wrapfd_listener_test),
	KUNIT_CASE(wrapfd_listener_file_release_test),
	KUNIT_CASE(wrapfd_listener_non_owner_test),
	KUNIT_CASE(wrapfd_listener_del_in_progress_test),
	KUNIT_CASE(is_wrapfd_file_test),
	KUNIT_CASE(wrapfd_allow_guests_test),
	{},
};

static struct kunit_suite wrapfd_test_module = {
	.name = "wrapfd",
	.init = wrapfd_test_init,
	.exit = wrapfd_test_exit,
	.suite_init = wrapfd_suite_init,
	.suite_exit = wrapfd_suite_exit,
	.test_cases = wrapfd_test_cases,
};

kunit_test_suites(&wrapfd_test_module);

MODULE_DESCRIPTION("KUnit test for the wrapfd driver");
MODULE_IMPORT_NS("DMA_BUF");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_IMPORT_NS("ANDROID_GKI_VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver");
MODULE_LICENSE("GPL");
