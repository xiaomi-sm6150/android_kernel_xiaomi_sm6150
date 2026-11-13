// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2014  Google, Inc.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/k61-stop.h>
#include "internal.h"

static DEFINE_MUTEX(pmsg_lock);

/*
 * DEBUG (k61.stop=sys): mirror warning-and-worse logcat lines to the kernel
 * log, so they survive in the pstore console. liblog writev()s each line as
 * [pmsg header][log header][prio][tag\0][msg\0]; without .write_iter the
 * VFS calls write_pmsg() once per segment. Remember the last one-byte
 * segment (the priority) per open file and print the text segments that
 * follow a W/E/F priority.
 */
static void k61_pmsg_mirror(struct file *file, const char __user *buf,
			    size_t count)
{
	unsigned long prio = (unsigned long)file->private_data;
	char text[256];
	size_t n, i;

	if (count == 1) {
		u8 c;

		if (!get_user(c, (const u8 __user *)buf))
			file->private_data = (void *)(unsigned long)c;
		return;
	}
	if (prio < 5 || prio > 8 || count < 2)
		return;
	n = min(count, sizeof(text) - 1);
	if (copy_from_user(text, buf, n))
		return;
	text[n] = 0;
	/* text only: binary headers (pmsg 'l' header, log header) are skipped */
	for (i = 0; i < n && text[i]; i++) {
		if (text[i] == '\n' || text[i] == '\t')
			text[i] = ' ';
		else if ((u8)text[i] < 0x20 || (u8)text[i] == 0x7f)
			return;
	}
	pr_info("k61logcat[%d %s] %c %s\n", task_tgid_nr(current), current->comm,
		"??VDIWEF"[prio], text);
}

static ssize_t write_pmsg(struct file *file, const char __user *buf,
			  size_t count, loff_t *ppos)
{
	struct pstore_record record;
	int ret;

	if (!count)
		return 0;

	if (k61_stop_sys)
		k61_pmsg_mirror(file, buf, count);

	pstore_record_init(&record, psinfo);
	record.type = PSTORE_TYPE_PMSG;
	record.size = count;

	/* check outside lock, page in any data. write_user also checks */
	if (!access_ok(buf, count))
		return -EFAULT;

	mutex_lock(&pmsg_lock);
	ret = psinfo->write_user(&record, buf);
	mutex_unlock(&pmsg_lock);
	return ret ? ret : count;
}

static const struct file_operations pmsg_fops = {
	.owner		= THIS_MODULE,
	.llseek		= noop_llseek,
	.write		= write_pmsg,
};

static struct class *pmsg_class;
static int pmsg_major;
#define PMSG_NAME "pmsg"
#undef pr_fmt
#define pr_fmt(fmt) PMSG_NAME ": " fmt

static char *pmsg_devnode(struct device *dev, umode_t *mode)
{
	if (mode)
		*mode = 0220;
	return NULL;
}

void pstore_register_pmsg(void)
{
	struct device *pmsg_device;

	pmsg_major = register_chrdev(0, PMSG_NAME, &pmsg_fops);
	if (pmsg_major < 0) {
		pr_err("register_chrdev failed\n");
		goto err;
	}

	pmsg_class = class_create(THIS_MODULE, PMSG_NAME);
	if (IS_ERR(pmsg_class)) {
		pr_err("device class file already in use\n");
		goto err_class;
	}
	pmsg_class->devnode = pmsg_devnode;

	pmsg_device = device_create(pmsg_class, NULL, MKDEV(pmsg_major, 0),
					NULL, "%s%d", PMSG_NAME, 0);
	if (IS_ERR(pmsg_device)) {
		pr_err("failed to create device\n");
		goto err_device;
	}
	return;

err_device:
	class_destroy(pmsg_class);
err_class:
	unregister_chrdev(pmsg_major, PMSG_NAME);
err:
	return;
}

void pstore_unregister_pmsg(void)
{
	device_destroy(pmsg_class, MKDEV(pmsg_major, 0));
	class_destroy(pmsg_class);
	unregister_chrdev(pmsg_major, PMSG_NAME);
}
