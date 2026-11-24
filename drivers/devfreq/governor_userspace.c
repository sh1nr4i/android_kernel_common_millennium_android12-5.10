// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/drivers/devfreq/governor_userspace.c
 *
 *  Copyright (C) 2011 Samsung Electronics
 *	MyungJoo Ham <myungjoo.ham@samsung.com>
 */

#include <linux/slab.h>
#include <linux/device.h>
#include <linux/devfreq.h>
#include <linux/kstrtox.h>
#include <linux/pm.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <linux/thermal.h>
#include "governor.h"

extern bool chihiro_is_gaming_active(void);

struct userspace_data {
	unsigned long user_frequency;
	bool valid;
};

static DEFINE_MUTEX(chihiro_dvfsrc_lock);
static struct devfreq *chihiro_dvfsrc_df;
static struct delayed_work chihiro_dvfsrc_monitor_work;
static bool chihiro_dvfsrc_work_init;
static bool chihiro_dvfsrc_boosted;
static bool chihiro_soc_cool = true;

bool chihiro_thermal_boost_allowed(void)
{
	return READ_ONCE(chihiro_soc_cool);
}

static bool chihiro_dvfsrc_thermal_ok(void)
{
	struct thermal_zone_device *soc_tz =
		thermal_zone_get_zone_by_name("soc_max");
	int temp = 0;
	bool ok = true;

	if (!IS_ERR(soc_tz) && thermal_zone_get_temp(soc_tz, &temp) == 0 &&
	    temp >= 82000)
		ok = false;
	WRITE_ONCE(chihiro_soc_cool, ok);
	return ok;
}

static void chihiro_dvfsrc_monitor_workfn(struct work_struct *work)
{
	struct devfreq *df;
	bool want_boost;

	mutex_lock(&chihiro_dvfsrc_lock);
	df = chihiro_dvfsrc_df;
	if (!df) {
		mutex_unlock(&chihiro_dvfsrc_lock);
		return;
	}

	want_boost = chihiro_is_gaming_active() && chihiro_dvfsrc_thermal_ok();

	if (want_boost) {
		mutex_lock(&df->lock);
		if (df->data) {
			struct userspace_data *data = df->data;

			if (!chihiro_dvfsrc_boosted ||
			    data->user_frequency != 4266000000UL) {
				data->user_frequency = 4266000000UL;
				data->valid = true;
				update_devfreq(df);
				chihiro_dvfsrc_boosted = true;
			}
		}
		mutex_unlock(&df->lock);
	} else if (chihiro_dvfsrc_boosted) {
		mutex_lock(&df->lock);
		if (df->data) {
			struct userspace_data *data = df->data;

			if (data->user_frequency == 4266000000UL) {
				data->user_frequency = 0;
				data->valid = true;
				update_devfreq(df);
			}
		}
		mutex_unlock(&df->lock);
		chihiro_dvfsrc_boosted = false;
	}

	queue_delayed_work(system_power_efficient_wq,
			   &chihiro_dvfsrc_monitor_work,
			   msecs_to_jiffies((want_boost || chihiro_dvfsrc_boosted) ?
					    1000 : 15000));
	mutex_unlock(&chihiro_dvfsrc_lock);
}

static int devfreq_userspace_func(struct devfreq *df, unsigned long *freq)
{
	struct userspace_data *data = df->data;

	if (data->valid)
		*freq = data->user_frequency;
	else
		*freq = df->previous_freq; /* No user freq specified yet */

	return 0;
}

static ssize_t store_freq(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct devfreq *devfreq = to_devfreq(dev);
	struct userspace_data *data;
	unsigned long wanted;
	int err = 0;

	err = kstrtoul(buf, 0, &wanted);
	if (err)
		return err;

	/*
	 * Keep LPDDR4X DRAM at 4266 MHz during active gameplay (SoC < 82C),
	 * overriding PowerHAL's 4-second reset to 0.
	 */
	if (wanted == 0 && dev_name(&devfreq->dev) &&
	    strstr(dev_name(&devfreq->dev), "dvfsrc") &&
	    chihiro_is_gaming_active() && chihiro_dvfsrc_thermal_ok()) {
		wanted = 4266000000UL;
		chihiro_dvfsrc_boosted = true;
	}

	mutex_lock(&devfreq->lock);
	data = devfreq->data;

	data->user_frequency = wanted;
	data->valid = true;
	err = update_devfreq(devfreq);
	if (err == 0)
		err = count;
	mutex_unlock(&devfreq->lock);
	return err;
}

static ssize_t show_freq(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct devfreq *devfreq = to_devfreq(dev);
	struct userspace_data *data;
	int err = 0;

	mutex_lock(&devfreq->lock);
	data = devfreq->data;

	if (data->valid)
		err = sprintf(buf, "%lu\n", data->user_frequency);
	else
		err = sprintf(buf, "undefined\n");
	mutex_unlock(&devfreq->lock);
	return err;
}

static DEVICE_ATTR(set_freq, 0644, show_freq, store_freq);
static struct attribute *dev_entries[] = {
	&dev_attr_set_freq.attr,
	NULL,
};
static const struct attribute_group dev_attr_group = {
	.name	= DEVFREQ_GOV_USERSPACE,
	.attrs	= dev_entries,
};

static int userspace_init(struct devfreq *devfreq)
{
	int err = 0;
	struct userspace_data *data = kzalloc(sizeof(struct userspace_data),
					      GFP_KERNEL);

	if (!data) {
		err = -ENOMEM;
		goto out;
	}
	data->valid = false;
	devfreq->data = data;

	err = sysfs_create_group(&devfreq->dev.kobj, &dev_attr_group);
	if (!err && dev_name(&devfreq->dev) &&
	    strstr(dev_name(&devfreq->dev), "dvfsrc")) {
		mutex_lock(&chihiro_dvfsrc_lock);
		chihiro_dvfsrc_df = devfreq;
		if (!chihiro_dvfsrc_work_init) {
			INIT_DEFERRABLE_WORK(&chihiro_dvfsrc_monitor_work,
					     chihiro_dvfsrc_monitor_workfn);
			chihiro_dvfsrc_work_init = true;
		}
		queue_delayed_work(system_power_efficient_wq,
				   &chihiro_dvfsrc_monitor_work,
				   msecs_to_jiffies(15000));
		mutex_unlock(&chihiro_dvfsrc_lock);
	}
out:
	return err;
}

static void userspace_exit(struct devfreq *devfreq)
{
	mutex_lock(&chihiro_dvfsrc_lock);
	if (chihiro_dvfsrc_df == devfreq) {
		chihiro_dvfsrc_df = NULL;
		chihiro_dvfsrc_boosted = false;
		if (chihiro_dvfsrc_work_init)
			cancel_delayed_work(&chihiro_dvfsrc_monitor_work);
	}
	mutex_unlock(&chihiro_dvfsrc_lock);

	/*
	 * Remove the sysfs entry, unless this is being called after
	 * device_del(), which should have done this already via kobject_del().
	 */
	if (devfreq->dev.kobj.sd)
		sysfs_remove_group(&devfreq->dev.kobj, &dev_attr_group);

	kfree(devfreq->data);
	devfreq->data = NULL;
}

static int devfreq_userspace_handler(struct devfreq *devfreq,
			unsigned int event, void *data)
{
	int ret = 0;

	switch (event) {
	case DEVFREQ_GOV_START:
		ret = userspace_init(devfreq);
		break;
	case DEVFREQ_GOV_STOP:
		userspace_exit(devfreq);
		break;
	default:
		break;
	}

	return ret;
}

static struct devfreq_governor devfreq_userspace = {
	.name = DEVFREQ_GOV_USERSPACE,
	.get_target_freq = devfreq_userspace_func,
	.event_handler = devfreq_userspace_handler,
};

static int __init devfreq_userspace_init(void)
{
	return devfreq_add_governor(&devfreq_userspace);
}
subsys_initcall(devfreq_userspace_init);

static void __exit devfreq_userspace_exit(void)
{
	int ret;

	ret = devfreq_remove_governor(&devfreq_userspace);
	if (ret)
		pr_err("%s: failed remove governor %d\n", __func__, ret);

	return;
}
module_exit(devfreq_userspace_exit);
MODULE_LICENSE("GPL");
