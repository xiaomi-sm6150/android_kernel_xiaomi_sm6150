// SPDX-License-Identifier: GPL-2.0-only
/*
 * msm_subsys sysfs shim for the 4.14 vendor userspace.
 *
 * libmdmdetect, pd-mapper and per_mgr look at /sys/bus/msm_subsys/devices/
 * subsysN/{name,state,...}. The 4.14 subsystem_restart driver provided them;
 * 6.1 has remoteproc instead, so mirror its SSR notifications here.
 */
#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/remoteproc/qcom_rproc.h>

struct k61_subsys {
	struct device dev;
	const char *name;	/* msm_subsys name (4.14) */
	const char *ssr_name;	/* remoteproc SSR name */
	struct notifier_block nb;
	void *notify;
	atomic_t online;
	atomic_t crashes;
	int restart_level;
};

static struct k61_subsys k61_subsystems[] = {
	{ .name = "adsp",  .ssr_name = "lpass" },
	{ .name = "modem", .ssr_name = "mpss" },
	{ .name = "cdsp",  .ssr_name = "cdsp" },
};

#define to_ss(d) container_of(d, struct k61_subsys, dev)

static ssize_t name_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%s\n", to_ss(dev)->name);
}
static DEVICE_ATTR_RO(name);

static ssize_t state_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%s\n",
			  atomic_read(&to_ss(dev)->online) ? "ONLINE" : "OFFLINE");
}
static DEVICE_ATTR_RO(state);

static ssize_t crash_count_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&to_ss(dev)->crashes));
}
static DEVICE_ATTR_RO(crash_count);

static ssize_t firmware_name_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%s\n", to_ss(dev)->name);
}
static ssize_t firmware_name_store(struct device *dev, struct device_attribute *a,
				   const char *buf, size_t count)
{
	return count;
}
static DEVICE_ATTR_RW(firmware_name);

static ssize_t restart_level_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%s\n", to_ss(dev)->restart_level ? "SYSTEM" : "RELATED");
}
static ssize_t restart_level_store(struct device *dev, struct device_attribute *a,
				   const char *buf, size_t count)
{
	to_ss(dev)->restart_level = !strncasecmp(buf, "SYSTEM", 6);
	return count;
}
static DEVICE_ATTR_RW(restart_level);

static ssize_t system_debug_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "disabled\n");
}
static ssize_t system_debug_store(struct device *dev, struct device_attribute *a,
				  const char *buf, size_t count)
{
	return count;
}
static DEVICE_ATTR_RW(system_debug);

static struct attribute *k61_subsys_attrs[] = {
	&dev_attr_name.attr,
	&dev_attr_state.attr,
	&dev_attr_crash_count.attr,
	&dev_attr_firmware_name.attr,
	&dev_attr_restart_level.attr,
	&dev_attr_system_debug.attr,
	NULL,
};
ATTRIBUTE_GROUPS(k61_subsys);

static struct bus_type k61_subsys_bus = {
	.name = "msm_subsys",
	.dev_groups = k61_subsys_groups,
};

static void k61_subsys_release(struct device *dev) { }

static int k61_ssr_cb(struct notifier_block *nb, unsigned long action, void *data)
{
	struct k61_subsys *ss = container_of(nb, struct k61_subsys, nb);
	struct qcom_ssr_notify_data *nd = data;

	switch (action) {
	case QCOM_SSR_AFTER_POWERUP:
		atomic_set(&ss->online, 1);
		break;
	case QCOM_SSR_BEFORE_SHUTDOWN:
		if (nd && nd->crashed)
			atomic_inc(&ss->crashes);
		atomic_set(&ss->online, 0);
		break;
	case QCOM_SSR_AFTER_SHUTDOWN:
		atomic_set(&ss->online, 0);
		break;
	}
	return NOTIFY_DONE;
}

static int __init k61_subsys_init(void)
{
	int i, ret;

	ret = bus_register(&k61_subsys_bus);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(k61_subsystems); i++) {
		struct k61_subsys *ss = &k61_subsystems[i];

		ss->dev.bus = &k61_subsys_bus;
		ss->dev.release = k61_subsys_release;
		dev_set_name(&ss->dev, "subsys%d", i);
		ret = device_register(&ss->dev);
		if (ret) {
			put_device(&ss->dev);
			continue;
		}
		ss->nb.notifier_call = k61_ssr_cb;
		ss->notify = qcom_register_ssr_notifier(ss->ssr_name, &ss->nb);
		if (IS_ERR(ss->notify))
			pr_warn("k61_subsys: no SSR notifier for %s\n", ss->ssr_name);
	}
	return 0;
}
late_initcall(k61_subsys_init);
