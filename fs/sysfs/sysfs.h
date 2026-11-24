/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fs/sysfs/sysfs.h - sysfs internal header file
 *
 * Copyright (c) 2001-3 Patrick Mochel
 * Copyright (c) 2007 SUSE Linux Products GmbH
 * Copyright (c) 2007 Tejun Heo <teheo@suse.de>
 */

#ifndef __SYSFS_INTERNAL_H
#define __SYSFS_INTERNAL_H

#include <linux/sysfs.h>

/*
 * mount.c
 */
extern struct kernfs_node *sysfs_root_kn;

/*
 * dir.c
 */
extern spinlock_t sysfs_symlink_target_lock;

void sysfs_warn_dup(struct kernfs_node *parent, const char *name);

/*
 * file.c
 */
static inline bool is_charging_sysfs_attr(const char *name)
{
	if (!name)
		return false;
	if (!strcmp(name, "bypass_charger") ||
	    !strcmp(name, "bypass_energy") ||
	    !strcmp(name, "cmd_discharging") ||
	    !strcmp(name, "adapter_capacity") ||
	    !strcmp(name, "High_voltage_chg_enable") ||
	    !strcmp(name, "enable_sc") ||
	    !strcmp(name, "Thermal_throttle") ||
	    !strcmp(name, "sc_ibat_limit") ||
	    !strcmp(name, "input_current") ||
	    !strcmp(name, "fast_chg_indicator") ||
	    !strcmp(name, "Pump_Express") ||
	    !strcmp(name, "Charging_mode") ||
	    !strcmp(name, "game_mode") ||
	    !strcmp(name, "stop_chg") ||
	    !strcmp(name, "chg_speed") ||
	    !strcmp(name, "quick_chg_back") ||
	    !strcmp(name, "fix_chg_type") ||
	    !strcmp(name, "tran_set_current") ||
	    !strcmp(name, "charging_current") ||
	    !strcmp(name, "current_max") ||
	    !strcmp(name, "input_current_limit") ||
	    !strcmp(name, "constant_charge_current") ||
	    !strcmp(name, "constant_charge_current_max") ||
	    !strcmp(name, "sc_tuisoc") ||
	    !strcmp(name, "sc_stime") ||
	    !strcmp(name, "sc_etime") ||
	    !strcmp(name, "sw_jeita") ||
	    !strcmp(name, "sw_ovp_threshold") ||
	    !strcmp(name, "tran_aichg_disable_charger") ||
	    !strcmp(name, "OTG_CTL") ||
	    !strcmp(name, "otg_debug") ||
	    !strcmp(name, "BatteryNotify"))
		return true;
	return false;
}

int sysfs_add_file_mode_ns(struct kernfs_node *parent,
			   const struct attribute *attr, bool is_bin,
			   umode_t amode, kuid_t uid, kgid_t gid,
			   const void *ns);

/*
 * symlink.c
 */
int sysfs_create_link_sd(struct kernfs_node *kn, struct kobject *target,
			 const char *name);

#endif	/* __SYSFS_INTERNAL_H */
