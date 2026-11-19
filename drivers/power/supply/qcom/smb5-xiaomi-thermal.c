// SPDX-License-Identifier: GPL-2.0-only
/*
 * Xiaomi per charger type thermal mitigation for the SMB5 charger.
 *
 * Xiaomi's 4.14 sm6150 charger (PixelOS-Devices android_kernel_xiaomi_sm6150
 * 06b9576aff33 "power: import Xiaomi changes") limits charging per thermal
 * level with a separate table for each charger type instead of the single
 * qcom,thermal-mitigation FCC table:
 *
 *   qcom,thermal-mitigation-dcp   ICL for DCP and other non HVDCP/PD chargers
 *   qcom,thermal-mitigation-qc2   ICL for QC 2.0
 *   qcom,thermal-fcc-qc3-normal   FCC for QC 3.0 without a charge pump
 *   qcom,thermal-mitigation-pd-base  ICL for PD at 5 V, scaled down for
 *                                 higher PD voltages
 *   qcom,thermal-mitigation-icl   extra ICL cap for QC 3.0 from level 4 up
 *
 * The top level stops charging. The charge pump (SMB1390/bq2597x) tables of
 * the 4.14 code are not used: no sm6150 device on this tree has a charge pump
 * driver yet.
 *
 * Unlike 4.14, each table is bounded by its own length: the tables differ in
 * size and 4.14 indexed all of them by the length of the last one parsed.
 *
 * The vendor thermal-engine writes the level to
 * /sys/class/power_supply/battery/system_temp_level, which 6.1 replaced with
 * charge_control_limit; provide the old node as well.
 */

#include <linux/of.h>
#include <linux/pmic-voter.h>
#include <linux/power_supply.h>
#include <linux/qti_power_supply.h>
#include "smb5-lib.h"

#define ICL_LIMIT_LEVEL_THR	4

/* PD ICL, in percent of the 5 V table, for higher PD voltage_min */
#define PD_MICRO_5P9V		5900000
#define PD_MICRO_6P5V		6500000
#define PD_MICRO_7P5V		7500000
#define PD_MICRO_8P5V		8500000
#define PD_6P5V_PERCENT		85
#define PD_7P5V_PERCENT		75
#define PD_8P5V_PERCENT		65
#define PD_9V_PERCENT		60

static int smb5_xm_read_table(struct smb_charger *chg, struct device_node *node,
			      const char *prop, struct smb_therm_table *t)
{
	int byte_len, rc;

	if (!of_find_property(node, prop, &byte_len) || byte_len < sizeof(u32))
		return 0;

	t->ua = devm_kzalloc(chg->dev, byte_len, GFP_KERNEL);
	if (!t->ua)
		return -ENOMEM;

	t->len = byte_len / sizeof(u32);
	rc = of_property_read_u32_array(node, prop, t->ua, t->len);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't read %s rc=%d\n", prop, rc);
		t->len = 0;
		return rc;
	}

	chg->xm_thermal = true;
	if (t->len > chg->thermal_levels)
		chg->thermal_levels = t->len;

	return 0;
}

int smb5_xm_thermal_parse_dt(struct smb_charger *chg, struct device_node *node)
{
	int rc;

	rc = smb5_xm_read_table(chg, node, "qcom,thermal-mitigation-dcp",
				&chg->therm_dcp);
	if (!rc)
		rc = smb5_xm_read_table(chg, node, "qcom,thermal-mitigation-qc2",
					&chg->therm_qc2);
	if (!rc)
		rc = smb5_xm_read_table(chg, node, "qcom,thermal-fcc-qc3-normal",
					&chg->therm_qc3);
	if (!rc)
		rc = smb5_xm_read_table(chg, node,
					"qcom,thermal-mitigation-pd-base",
					&chg->therm_pd);
	if (!rc)
		rc = smb5_xm_read_table(chg, node, "qcom,thermal-mitigation-icl",
					&chg->therm_icl);

	return rc;
}

static int smb5_xm_table(const struct smb_therm_table *t, int level)
{
	if (!t->len)
		return 0;

	return t->ua[min(level, t->len - 1)];
}

static int smb5_xm_pd_icl(struct smb_charger *chg, int level)
{
	int icl = smb5_xm_table(&chg->therm_pd, level);
	int uv = chg->voltage_min_uv;

	if (uv >= PD_MICRO_8P5V)
		return icl * PD_9V_PERCENT / 100;
	if (uv >= PD_MICRO_7P5V)
		return icl * PD_8P5V_PERCENT / 100;
	if (uv >= PD_MICRO_6P5V)
		return icl * PD_7P5V_PERCENT / 100;
	if (uv >= PD_MICRO_5P9V)
		return icl * PD_6P5V_PERCENT / 100;

	return icl;
}

/*
 * Vote the limits of the current thermal level for the current charger type.
 * Called when the level changes and when the charger type becomes known.
 */
void smb5_xm_thermal_apply(struct smb_charger *chg)
{
	int level = chg->system_temp_level;
	int icl = 0, fcc = 0;
	bool qc3 = false;

	if (!chg->xm_thermal)
		return;

	if (level <= 0) {
		vote(chg->usb_icl_votable, THERMAL_DAEMON_VOTER, false, 0);
		vote(chg->fcc_votable, THERMAL_DAEMON_VOTER, false, 0);
		return;
	}

	/* the top level disables charging, see smb5_xm_set_level() */
	if (level >= chg->thermal_levels - 1)
		return;

	switch (chg->real_charger_type) {
	case QTI_POWER_SUPPLY_TYPE_USB_HVDCP:
		icl = smb5_xm_table(&chg->therm_qc2, level);
		break;
	case QTI_POWER_SUPPLY_TYPE_USB_HVDCP_3:
	case QTI_POWER_SUPPLY_TYPE_USB_HVDCP_3P5:
		qc3 = true;
		fcc = smb5_xm_table(&chg->therm_qc3, level);
		if (level >= ICL_LIMIT_LEVEL_THR)
			icl = smb5_xm_table(&chg->therm_icl, level);
		break;
	case POWER_SUPPLY_TYPE_USB_PD:
		icl = smb5_xm_pd_icl(chg, level);
		break;
	default:
		icl = smb5_xm_table(&chg->therm_dcp, level);
		break;
	}

	pr_debug("%s: thermal level %d type %d: icl %d fcc %d%s\n", chg->name,
		 level, chg->real_charger_type, icl, fcc, qc3 ? " (qc3)" : "");

	if (icl > 0)
		vote(chg->usb_icl_votable, THERMAL_DAEMON_VOTER, true, icl);
	else
		vote(chg->usb_icl_votable, THERMAL_DAEMON_VOTER, false, 0);

	if (fcc > 0)
		vote(chg->fcc_votable, THERMAL_DAEMON_VOTER, true, fcc);
	else
		vote(chg->fcc_votable, THERMAL_DAEMON_VOTER, false, 0);
}

int smb5_xm_set_level(struct smb_charger *chg, int level)
{
	chg->system_temp_level = level;

	if (level >= chg->thermal_levels - 1)
		return vote(chg->chg_disable_votable, THERMAL_DAEMON_VOTER,
			    true, 0);

	vote(chg->chg_disable_votable, THERMAL_DAEMON_VOTER, false, 0);
	smb5_xm_thermal_apply(chg);

	return 0;
}

static ssize_t system_temp_level_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct smb_charger *chg = power_supply_get_drvdata(to_power_supply(dev));

	return sysfs_emit(buf, "%d\n", chg->system_temp_level);
}

static ssize_t system_temp_level_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct smb_charger *chg = power_supply_get_drvdata(to_power_supply(dev));
	union power_supply_propval val;
	int rc;

	rc = kstrtoint(buf, 0, &val.intval);
	if (rc)
		return rc;

	rc = smblib_set_prop_system_temp_level(chg, &val);
	if (rc < 0)
		return rc;

	return count;
}
static DEVICE_ATTR_RW(system_temp_level);

static struct attribute *smb5_xm_batt_attrs[] = {
	&dev_attr_system_temp_level.attr,
	NULL,
};

const struct attribute_group smb5_xm_batt_group = {
	.attrs = smb5_xm_batt_attrs,
};
