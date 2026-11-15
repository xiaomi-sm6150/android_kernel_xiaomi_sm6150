// SPDX-License-Identifier: GPL-2.0-only
/*
 * /dev/wlan driver-state stub.
 *
 * Android Wi-Fi HALs built for the downstream qcacld driver
 * (WIFI_DRIVER_STATE_CTRL_PARAM := "/dev/wlan") write "ON"/"OFF" to
 * /dev/wlan before using the interface and refuse to start when the node
 * is missing. With ath10k the interface is always present, so accept and
 * ignore those writes. This keeps a vendor/ROM built for qcacld working.
 */

#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/uaccess.h>

static ssize_t wlan_state_write(struct file *file, const char __user *buf,
				size_t count, loff_t *ppos)
{
	char state[8] = {};

	if (copy_from_user(state, buf, min(count, sizeof(state) - 1)))
		return -EFAULT;

	pr_debug("wlan-state-stub: driver state \"%s\"\n", strim(state));

	return count;
}

static const struct file_operations wlan_state_fops = {
	.owner = THIS_MODULE,
	.open = nonseekable_open,
	.write = wlan_state_write,
	.llseek = no_llseek,
};

static struct miscdevice wlan_state_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "wlan",
	.fops = &wlan_state_fops,
};

module_misc_device(wlan_state_dev);

MODULE_DESCRIPTION("/dev/wlan driver-state stub for qcacld-built Android Wi-Fi HALs");
MODULE_LICENSE("GPL");
