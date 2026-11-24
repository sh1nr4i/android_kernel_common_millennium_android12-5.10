// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Universal power supply monitor class
 *
 *  Copyright © 2007  Anton Vorontsov <cbou@mail.ru>
 *  Copyright © 2004  Szabolcs Gyurko
 *  Copyright © 2003  Ian Molton <spyro@f2s.com>
 *
 *  Modified: 2004, Oct     Szabolcs Gyurko
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/notifier.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/thermal.h>
#include <linux/kallsyms.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include "power_supply.h"

/* exported for the APM Power driver, APM emulation */
struct class *power_supply_class;
EXPORT_SYMBOL_GPL(power_supply_class);

ATOMIC_NOTIFIER_HEAD(power_supply_notifier);
EXPORT_SYMBOL_GPL(power_supply_notifier);

static struct device_type power_supply_dev_type;

struct match_device_node_array_param {
	struct device_node *parent_of_node;
	struct power_supply **psy;
	ssize_t psy_size;
	ssize_t psy_count;
};

#define POWER_SUPPLY_DEFERRED_REGISTER_TIME	msecs_to_jiffies(10)

static bool __power_supply_is_supplied_by(struct power_supply *supplier,
					 struct power_supply *supply)
{
	int i;

	if (!supply->supplied_from && !supplier->supplied_to)
		return false;

	/* Support both supplied_to and supplied_from modes */
	if (supply->supplied_from) {
		if (!supplier->desc->name)
			return false;
		for (i = 0; i < supply->num_supplies; i++)
			if (!strcmp(supplier->desc->name, supply->supplied_from[i]))
				return true;
	} else {
		if (!supply->desc->name)
			return false;
		for (i = 0; i < supplier->num_supplicants; i++)
			if (!strcmp(supplier->supplied_to[i], supply->desc->name))
				return true;
	}

	return false;
}

static int __power_supply_changed_work(struct device *dev, void *data)
{
	struct power_supply *psy = data;
	struct power_supply *pst = dev_get_drvdata(dev);

	if (__power_supply_is_supplied_by(psy, pst)) {
		if (pst->desc->external_power_changed)
			pst->desc->external_power_changed(pst);
	}

	return 0;
}

static void power_supply_changed_work(struct work_struct *work)
{
	unsigned long flags;
	struct power_supply *psy = container_of(work, struct power_supply,
						changed_work);

	dev_dbg(&psy->dev, "%s\n", __func__);

	spin_lock_irqsave(&psy->changed_lock, flags);
	/*
	 * Check 'changed' here to avoid issues due to race between
	 * power_supply_changed() and this routine. In worst case
	 * power_supply_changed() can be called again just before we take above
	 * lock. During the first call of this routine we will mark 'changed' as
	 * false and it will stay false for the next call as well.
	 */
	if (likely(psy->changed)) {
		psy->changed = false;
		spin_unlock_irqrestore(&psy->changed_lock, flags);
		class_for_each_device(power_supply_class, NULL, psy,
				      __power_supply_changed_work);
		power_supply_update_leds(psy);
		atomic_notifier_call_chain(&power_supply_notifier,
				PSY_EVENT_PROP_CHANGED, psy);
		kobject_uevent(&psy->dev.kobj, KOBJ_CHANGE);
		spin_lock_irqsave(&psy->changed_lock, flags);
	}

	/*
	 * Hold the wakeup_source until all events are processed.
	 * power_supply_changed() might have called again and have set 'changed'
	 * to true.
	 */
	if (likely(!psy->changed))
		pm_relax(&psy->dev);
	spin_unlock_irqrestore(&psy->changed_lock, flags);
}

void power_supply_changed(struct power_supply *psy)
{
	unsigned long flags;

	dev_dbg(&psy->dev, "%s\n", __func__);

	spin_lock_irqsave(&psy->changed_lock, flags);
	psy->changed = true;
	pm_stay_awake(&psy->dev);
	spin_unlock_irqrestore(&psy->changed_lock, flags);
	schedule_work(&psy->changed_work);
}
EXPORT_SYMBOL_GPL(power_supply_changed);

static int psy_register_cooler(struct power_supply *psy);
/*
 * Notify that power supply was registered after parent finished the probing.
 *
 * Often power supply is registered from driver's probe function. However
 * calling power_supply_changed() directly from power_supply_register()
 * would lead to execution of get_property() function provided by the driver
 * too early - before the probe ends.
 * Also, registering cooling device from the probe will execute the
 * get_property() function. So register the cooling device after the probe.
 *
 * Avoid that by waiting on parent's mutex.
 */
static void power_supply_deferred_register_work(struct work_struct *work)
{
	struct power_supply *psy = container_of(work, struct power_supply,
						deferred_register_work.work);

	if (psy->dev.parent) {
		while (!mutex_trylock(&psy->dev.parent->mutex)) {
			if (psy->removing)
				return;
			msleep(10);
		}
	}

	power_supply_changed(psy);
	psy_register_cooler(psy);

	if (psy->dev.parent)
		mutex_unlock(&psy->dev.parent->mutex);
}

#ifdef CONFIG_OF
static int __power_supply_populate_supplied_from(struct device *dev,
						 void *data)
{
	struct power_supply *psy = data;
	struct power_supply *epsy = dev_get_drvdata(dev);
	struct device_node *np;
	int i = 0;

	do {
		np = of_parse_phandle(psy->of_node, "power-supplies", i++);
		if (!np)
			break;

		if (np == epsy->of_node) {
			dev_info(&psy->dev, "%s: Found supply : %s\n",
				psy->desc->name, epsy->desc->name);
			psy->supplied_from[i-1] = (char *)epsy->desc->name;
			psy->num_supplies++;
			of_node_put(np);
			break;
		}
		of_node_put(np);
	} while (np);

	return 0;
}

static int power_supply_populate_supplied_from(struct power_supply *psy)
{
	int error;

	error = class_for_each_device(power_supply_class, NULL, psy,
				      __power_supply_populate_supplied_from);

	dev_dbg(&psy->dev, "%s %d\n", __func__, error);

	return error;
}

static int  __power_supply_find_supply_from_node(struct device *dev,
						 void *data)
{
	struct device_node *np = data;
	struct power_supply *epsy = dev_get_drvdata(dev);

	/* returning non-zero breaks out of class_for_each_device loop */
	if (epsy->of_node == np)
		return 1;

	return 0;
}

static int power_supply_find_supply_from_node(struct device_node *supply_node)
{
	int error;

	/*
	 * class_for_each_device() either returns its own errors or values
	 * returned by __power_supply_find_supply_from_node().
	 *
	 * __power_supply_find_supply_from_node() will return 0 (no match)
	 * or 1 (match).
	 *
	 * We return 0 if class_for_each_device() returned 1, -EPROBE_DEFER if
	 * it returned 0, or error as returned by it.
	 */
	error = class_for_each_device(power_supply_class, NULL, supply_node,
				       __power_supply_find_supply_from_node);

	return error ? (error == 1 ? 0 : error) : -EPROBE_DEFER;
}

static int power_supply_check_supplies(struct power_supply *psy)
{
	struct device_node *np;
	int cnt = 0;

	/* If there is already a list honor it */
	if (psy->supplied_from && psy->num_supplies > 0)
		return 0;

	/* No device node found, nothing to do */
	if (!psy->of_node)
		return 0;

	do {
		int ret;

		np = of_parse_phandle(psy->of_node, "power-supplies", cnt++);
		if (!np)
			break;

		ret = power_supply_find_supply_from_node(np);
		of_node_put(np);

		if (ret) {
			dev_dbg(&psy->dev, "Failed to find supply!\n");
			return ret;
		}
	} while (np);

	/* Missing valid "power-supplies" entries */
	if (cnt == 1)
		return 0;

	/* All supplies found, allocate char ** array for filling */
	psy->supplied_from = devm_kzalloc(&psy->dev, sizeof(psy->supplied_from),
					  GFP_KERNEL);
	if (!psy->supplied_from)
		return -ENOMEM;

	*psy->supplied_from = devm_kcalloc(&psy->dev,
					   cnt - 1, sizeof(char *),
					   GFP_KERNEL);
	if (!*psy->supplied_from)
		return -ENOMEM;

	return power_supply_populate_supplied_from(psy);
}
#else
static int power_supply_check_supplies(struct power_supply *psy)
{
	int nval, ret;

	if (!psy->dev.parent)
		return 0;

	nval = device_property_read_string_array(psy->dev.parent,
						 "supplied-from", NULL, 0);
	if (nval <= 0)
		return 0;

	psy->supplied_from = devm_kmalloc_array(&psy->dev, nval,
						sizeof(char *), GFP_KERNEL);
	if (!psy->supplied_from)
		return -ENOMEM;

	ret = device_property_read_string_array(psy->dev.parent,
		"supplied-from", (const char **)psy->supplied_from, nval);
	if (ret < 0)
		return ret;

	psy->num_supplies = nval;

	return 0;
}
#endif

struct psy_am_i_supplied_data {
	struct power_supply *psy;
	unsigned int count;
};

static int __power_supply_am_i_supplied(struct device *dev, void *_data)
{
	union power_supply_propval ret = {0,};
	struct power_supply *epsy = dev_get_drvdata(dev);
	struct psy_am_i_supplied_data *data = _data;

	if (__power_supply_is_supplied_by(epsy, data->psy)) {
		data->count++;
		if (!epsy->desc->get_property(epsy, POWER_SUPPLY_PROP_ONLINE,
					&ret))
			return ret.intval;
	}

	return 0;
}

int power_supply_am_i_supplied(struct power_supply *psy)
{
	struct psy_am_i_supplied_data data = { psy, 0 };
	int error;

	error = class_for_each_device(power_supply_class, NULL, &data,
				      __power_supply_am_i_supplied);

	dev_dbg(&psy->dev, "%s count %u err %d\n", __func__, data.count, error);

	if (data.count == 0)
		return -ENODEV;

	return error;
}
EXPORT_SYMBOL_GPL(power_supply_am_i_supplied);

static int __power_supply_is_system_supplied(struct device *dev, void *data)
{
	union power_supply_propval ret = {0,};
	struct power_supply *psy = dev_get_drvdata(dev);
	unsigned int *count = data;

	if (!psy->desc->get_property(psy, POWER_SUPPLY_PROP_SCOPE, &ret))
		if (ret.intval == POWER_SUPPLY_SCOPE_DEVICE)
			return 0;

	(*count)++;
	if (psy->desc->type != POWER_SUPPLY_TYPE_BATTERY)
		if (!psy->desc->get_property(psy, POWER_SUPPLY_PROP_ONLINE,
					&ret))
			return ret.intval;

	return 0;
}

int power_supply_is_system_supplied(void)
{
	int error;
	unsigned int count = 0;

	error = class_for_each_device(power_supply_class, NULL, &count,
				      __power_supply_is_system_supplied);

	/*
	 * If no system scope power class device was found at all, most probably we
	 * are running on a desktop system, so assume we are on mains power.
	 */
	if (count == 0)
		return 1;

	return error;
}
EXPORT_SYMBOL_GPL(power_supply_is_system_supplied);

struct psy_get_supplier_prop_data {
	struct power_supply *psy;
	enum power_supply_property psp;
	union power_supply_propval *val;
};

static int __power_supply_get_supplier_property(struct device *dev, void *_data)
{
	struct power_supply *epsy = dev_get_drvdata(dev);
	struct psy_get_supplier_prop_data *data = _data;

	if (__power_supply_is_supplied_by(epsy, data->psy))
		if (!epsy->desc->get_property(epsy, data->psp, data->val))
			return 1; /* Success */

	return 0; /* Continue iterating */
}

int power_supply_get_property_from_supplier(struct power_supply *psy,
					    enum power_supply_property psp,
					    union power_supply_propval *val)
{
	struct psy_get_supplier_prop_data data = {
		.psy = psy,
		.psp = psp,
		.val = val,
	};
	int ret;

	/*
	 * This function is not intended for use with a supply with multiple
	 * suppliers, we simply pick the first supply to report the psp.
	 */
	ret = class_for_each_device(power_supply_class, NULL, &data,
				    __power_supply_get_supplier_property);
	if (ret < 0)
		return ret;
	if (ret == 0)
		return -ENODEV;

	return 0;
}
EXPORT_SYMBOL_GPL(power_supply_get_property_from_supplier);

int power_supply_set_battery_charged(struct power_supply *psy)
{
	if (atomic_read(&psy->use_cnt) >= 0 &&
			psy->desc->type == POWER_SUPPLY_TYPE_BATTERY &&
			psy->desc->set_charged) {
		psy->desc->set_charged(psy);
		return 0;
	}

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(power_supply_set_battery_charged);

static int power_supply_match_device_by_name(struct device *dev, const void *data)
{
	const char *name = data;
	struct power_supply *psy = dev_get_drvdata(dev);

	return strcmp(psy->desc->name, name) == 0;
}

/**
 * power_supply_get_by_name() - Search for a power supply and returns its ref
 * @name: Power supply name to fetch
 *
 * If power supply was found, it increases reference count for the
 * internal power supply's device. The user should power_supply_put()
 * after usage.
 *
 * Return: On success returns a reference to a power supply with
 * matching name equals to @name, a NULL otherwise.
 */
struct power_supply *power_supply_get_by_name(const char *name)
{
	struct power_supply *psy = NULL;
	struct device *dev = class_find_device(power_supply_class, NULL, name,
					power_supply_match_device_by_name);

	if (dev) {
		psy = dev_get_drvdata(dev);
		atomic_inc(&psy->use_cnt);
	}

	return psy;
}
EXPORT_SYMBOL_GPL(power_supply_get_by_name);

/**
 * power_supply_put() - Drop reference obtained with power_supply_get_by_name
 * @psy: Reference to put
 *
 * The reference to power supply should be put before unregistering
 * the power supply.
 */
void power_supply_put(struct power_supply *psy)
{
	atomic_dec(&psy->use_cnt);
	put_device(&psy->dev);
}
EXPORT_SYMBOL_GPL(power_supply_put);

#ifdef CONFIG_OF
static int power_supply_match_device_node(struct device *dev, const void *data)
{
	return dev->parent && dev->parent->of_node == data;
}

/**
 * power_supply_get_by_phandle() - Search for a power supply and returns its ref
 * @np: Pointer to device node holding phandle property
 * @property: Name of property holding a power supply name
 *
 * If power supply was found, it increases reference count for the
 * internal power supply's device. The user should power_supply_put()
 * after usage.
 *
 * Return: On success returns a reference to a power supply with
 * matching name equals to value under @property, NULL or ERR_PTR otherwise.
 */
struct power_supply *power_supply_get_by_phandle(struct device_node *np,
							const char *property)
{
	struct device_node *power_supply_np;
	struct power_supply *psy = NULL;
	struct device *dev;

	power_supply_np = of_parse_phandle(np, property, 0);
	if (!power_supply_np)
		return ERR_PTR(-ENODEV);

	dev = class_find_device(power_supply_class, NULL, power_supply_np,
						power_supply_match_device_node);

	of_node_put(power_supply_np);

	if (dev) {
		psy = dev_get_drvdata(dev);
		atomic_inc(&psy->use_cnt);
	}

	return psy;
}
EXPORT_SYMBOL_GPL(power_supply_get_by_phandle);

static int power_supply_match_device_node_array(struct device *dev,
						void *data)
{
	struct match_device_node_array_param *param =
		(struct match_device_node_array_param *)data;
	struct power_supply **psy = param->psy;
	ssize_t size = param->psy_size;
	ssize_t *count = &param->psy_count;

	if (!dev->parent || dev->parent->of_node != param->parent_of_node)
		return 0;

	if (*count >= size)
		return -EOVERFLOW;

	psy[*count] = dev_get_drvdata(dev);
	atomic_inc(&psy[*count]->use_cnt);
	(*count)++;

	return 0;
}

/**
 * power_supply_get_by_phandle_array() - Similar to
 * power_supply_get_by_phandle but returns an array of power supply
 * objects which are associated with the phandle.
 * @np: Pointer to device node holding phandle property.
 * @property: Name of property holding a power supply name.
 * @psy: Array of power_supply pointers provided by the client which is
 * filled by power_supply_get_by_phandle_array.
 * @size: size of power_supply pointer array.
 *
 * If power supply was found, it increases reference count for the
 * internal power supply's device. The user should power_supply_put()
 * after usage.
 *
 * Return: On success returns the number of power supply objects filled
 * in the @psy array.
 * -EOVERFLOW when size of @psy array is not suffice.
 * -EINVAL when @psy is NULL or @size is 0.
 * -ENODEV when matching device_node is not found.
 */
int power_supply_get_by_phandle_array(struct device_node *np,
				      const char *property,
				      struct power_supply **psy,
				      ssize_t size)
{
	struct device_node *power_supply_np;
	int ret;
	struct match_device_node_array_param param;

	if (!psy || !size)
		return -EINVAL;

	power_supply_np = of_parse_phandle(np, property, 0);
	if (!power_supply_np)
		return -ENODEV;

	param.parent_of_node = power_supply_np;
	param.psy = psy;
	param.psy_size = size;
	param.psy_count = 0;
	ret = class_for_each_device(power_supply_class, NULL, &param,
				    power_supply_match_device_node_array);

	of_node_put(power_supply_np);

	return param.psy_count;
}
EXPORT_SYMBOL_GPL(power_supply_get_by_phandle_array);

static void devm_power_supply_put(struct device *dev, void *res)
{
	struct power_supply **psy = res;

	power_supply_put(*psy);
}

/**
 * devm_power_supply_get_by_phandle() - Resource managed version of
 *  power_supply_get_by_phandle()
 * @dev: Pointer to device holding phandle property
 * @property: Name of property holding a power supply phandle
 *
 * Return: On success returns a reference to a power supply with
 * matching name equals to value under @property, NULL or ERR_PTR otherwise.
 */
struct power_supply *devm_power_supply_get_by_phandle(struct device *dev,
						      const char *property)
{
	struct power_supply **ptr, *psy;

	if (!dev->of_node)
		return ERR_PTR(-ENODEV);

	ptr = devres_alloc(devm_power_supply_put, sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		return ERR_PTR(-ENOMEM);

	psy = power_supply_get_by_phandle(dev->of_node, property);
	if (IS_ERR_OR_NULL(psy)) {
		devres_free(ptr);
	} else {
		*ptr = psy;
		devres_add(dev, ptr);
	}
	return psy;
}
EXPORT_SYMBOL_GPL(devm_power_supply_get_by_phandle);
#endif /* CONFIG_OF */

int power_supply_get_battery_info(struct power_supply *psy,
				  struct power_supply_battery_info *info)
{
	struct power_supply_resistance_temp_table *resist_table;
	struct device_node *battery_np;
	const char *value;
	int err, len, index;
	const __be32 *list;

	info->energy_full_design_uwh         = -EINVAL;
	info->charge_full_design_uah         = -EINVAL;
	info->voltage_min_design_uv          = -EINVAL;
	info->voltage_max_design_uv          = -EINVAL;
	info->precharge_current_ua           = -EINVAL;
	info->charge_term_current_ua         = -EINVAL;
	info->constant_charge_current_max_ua = -EINVAL;
	info->constant_charge_voltage_max_uv = -EINVAL;
	info->temp_ambient_alert_min         = INT_MIN;
	info->temp_ambient_alert_max         = INT_MAX;
	info->temp_alert_min                 = INT_MIN;
	info->temp_alert_max                 = INT_MAX;
	info->temp_min                       = INT_MIN;
	info->temp_max                       = INT_MAX;
	info->factory_internal_resistance_uohm  = -EINVAL;
	info->resist_table = NULL;

	for (index = 0; index < POWER_SUPPLY_OCV_TEMP_MAX; index++) {
		info->ocv_table[index]       = NULL;
		info->ocv_temp[index]        = -EINVAL;
		info->ocv_table_size[index]  = -EINVAL;
	}

	if (!psy->of_node) {
		dev_warn(&psy->dev, "%s currently only supports devicetree\n",
			 __func__);
		return -ENXIO;
	}

	battery_np = of_parse_phandle(psy->of_node, "monitored-battery", 0);
	if (!battery_np)
		return -ENODEV;

	err = of_property_read_string(battery_np, "compatible", &value);
	if (err)
		goto out_put_node;

	if (strcmp("simple-battery", value)) {
		err = -ENODEV;
		goto out_put_node;
	}

	/* The property and field names below must correspond to elements
	 * in enum power_supply_property. For reasoning, see
	 * Documentation/power/power_supply_class.rst.
	 */

	of_property_read_u32(battery_np, "energy-full-design-microwatt-hours",
			     &info->energy_full_design_uwh);
	of_property_read_u32(battery_np, "charge-full-design-microamp-hours",
			     &info->charge_full_design_uah);
	of_property_read_u32(battery_np, "voltage-min-design-microvolt",
			     &info->voltage_min_design_uv);
	of_property_read_u32(battery_np, "voltage-max-design-microvolt",
			     &info->voltage_max_design_uv);
	of_property_read_u32(battery_np, "trickle-charge-current-microamp",
			     &info->tricklecharge_current_ua);
	of_property_read_u32(battery_np, "precharge-current-microamp",
			     &info->precharge_current_ua);
	of_property_read_u32(battery_np, "precharge-upper-limit-microvolt",
			     &info->precharge_voltage_max_uv);
	of_property_read_u32(battery_np, "charge-term-current-microamp",
			     &info->charge_term_current_ua);
	of_property_read_u32(battery_np, "re-charge-voltage-microvolt",
			     &info->charge_restart_voltage_uv);
	of_property_read_u32(battery_np, "over-voltage-threshold-microvolt",
			     &info->overvoltage_limit_uv);
	of_property_read_u32(battery_np, "constant-charge-current-max-microamp",
			     &info->constant_charge_current_max_ua);
	of_property_read_u32(battery_np, "constant-charge-voltage-max-microvolt",
			     &info->constant_charge_voltage_max_uv);
	of_property_read_u32(battery_np, "factory-internal-resistance-micro-ohms",
			     &info->factory_internal_resistance_uohm);

	of_property_read_u32_index(battery_np, "ambient-celsius",
				   0, &info->temp_ambient_alert_min);
	of_property_read_u32_index(battery_np, "ambient-celsius",
				   1, &info->temp_ambient_alert_max);
	of_property_read_u32_index(battery_np, "alert-celsius",
				   0, &info->temp_alert_min);
	of_property_read_u32_index(battery_np, "alert-celsius",
				   1, &info->temp_alert_max);
	of_property_read_u32_index(battery_np, "operating-range-celsius",
				   0, &info->temp_min);
	of_property_read_u32_index(battery_np, "operating-range-celsius",
				   1, &info->temp_max);

	len = of_property_count_u32_elems(battery_np, "ocv-capacity-celsius");
	if (len < 0 && len != -EINVAL) {
		err = len;
		goto out_put_node;
	} else if (len > POWER_SUPPLY_OCV_TEMP_MAX) {
		dev_err(&psy->dev, "Too many temperature values\n");
		err = -EINVAL;
		goto out_put_node;
	} else if (len > 0) {
		of_property_read_u32_array(battery_np, "ocv-capacity-celsius",
					   info->ocv_temp, len);
	}

	for (index = 0; index < len; index++) {
		struct power_supply_battery_ocv_table *table;
		char *propname;
		int i, tab_len, size;

		propname = kasprintf(GFP_KERNEL, "ocv-capacity-table-%d", index);
		if (!propname) {
			power_supply_put_battery_info(psy, info);
			err = -ENOMEM;
			goto out_put_node;
		}
		list = of_get_property(battery_np, propname, &size);
		if (!list || !size) {
			dev_err(&psy->dev, "failed to get %s\n", propname);
			kfree(propname);
			power_supply_put_battery_info(psy, info);
			err = -EINVAL;
			goto out_put_node;
		}

		kfree(propname);
		tab_len = size / (2 * sizeof(__be32));
		info->ocv_table_size[index] = tab_len;

		table = info->ocv_table[index] =
			devm_kcalloc(&psy->dev, tab_len, sizeof(*table), GFP_KERNEL);
		if (!info->ocv_table[index]) {
			power_supply_put_battery_info(psy, info);
			err = -ENOMEM;
			goto out_put_node;
		}

		for (i = 0; i < tab_len; i++) {
			table[i].ocv = be32_to_cpu(*list);
			list++;
			table[i].capacity = be32_to_cpu(*list);
			list++;
		}
	}

	list = of_get_property(battery_np, "resistance-temp-table", &len);
	if (!list || !len)
		goto out_put_node;

	info->resist_table_size = len / (2 * sizeof(__be32));
	resist_table = info->resist_table = devm_kcalloc(&psy->dev,
							 info->resist_table_size,
							 sizeof(*resist_table),
							 GFP_KERNEL);
	if (!info->resist_table) {
		power_supply_put_battery_info(psy, info);
		err = -ENOMEM;
		goto out_put_node;
	}

	for (index = 0; index < info->resist_table_size; index++) {
		resist_table[index].temp = be32_to_cpu(*list++);
		resist_table[index].resistance = be32_to_cpu(*list++);
	}

out_put_node:
	of_node_put(battery_np);
	return err;
}
EXPORT_SYMBOL_GPL(power_supply_get_battery_info);

void power_supply_put_battery_info(struct power_supply *psy,
				   struct power_supply_battery_info *info)
{
	int i;

	for (i = 0; i < POWER_SUPPLY_OCV_TEMP_MAX; i++) {
		if (info->ocv_table[i])
			devm_kfree(&psy->dev, info->ocv_table[i]);
	}

	if (info->resist_table)
		devm_kfree(&psy->dev, info->resist_table);
}
EXPORT_SYMBOL_GPL(power_supply_put_battery_info);

/**
 * power_supply_temp2resist_simple() - find the battery internal resistance
 * percent
 * @table: Pointer to battery resistance temperature table
 * @table_len: The table length
 * @temp: Current temperature
 *
 * This helper function is used to look up battery internal resistance percent
 * according to current temperature value from the resistance temperature table,
 * and the table must be ordered descending. Then the actual battery internal
 * resistance = the ideal battery internal resistance * percent / 100.
 *
 * Return: the battery internal resistance percent
 */
int power_supply_temp2resist_simple(struct power_supply_resistance_temp_table *table,
				    int table_len, int temp)
{
	int i, resist;

	for (i = 0; i < table_len; i++)
		if (temp > table[i].temp)
			break;

	if (i > 0 && i < table_len) {
		int tmp;

		tmp = (table[i - 1].resistance - table[i].resistance) *
			(temp - table[i].temp);
		tmp /= table[i - 1].temp - table[i].temp;
		resist = tmp + table[i].resistance;
	} else if (i == 0) {
		resist = table[0].resistance;
	} else {
		resist = table[table_len - 1].resistance;
	}

	return resist;
}
EXPORT_SYMBOL_GPL(power_supply_temp2resist_simple);

/**
 * power_supply_ocv2cap_simple() - find the battery capacity
 * @table: Pointer to battery OCV lookup table
 * @table_len: OCV table length
 * @ocv: Current OCV value
 *
 * This helper function is used to look up battery capacity according to
 * current OCV value from one OCV table, and the OCV table must be ordered
 * descending.
 *
 * Return: the battery capacity.
 */
int power_supply_ocv2cap_simple(struct power_supply_battery_ocv_table *table,
				int table_len, int ocv)
{
	int i, cap, tmp;

	for (i = 0; i < table_len; i++)
		if (ocv > table[i].ocv)
			break;

	if (i > 0 && i < table_len) {
		tmp = (table[i - 1].capacity - table[i].capacity) *
			(ocv - table[i].ocv);
		tmp /= table[i - 1].ocv - table[i].ocv;
		cap = tmp + table[i].capacity;
	} else if (i == 0) {
		cap = table[0].capacity;
	} else {
		cap = table[table_len - 1].capacity;
	}

	return cap;
}
EXPORT_SYMBOL_GPL(power_supply_ocv2cap_simple);

struct power_supply_battery_ocv_table *
power_supply_find_ocv2cap_table(struct power_supply_battery_info *info,
				int temp, int *table_len)
{
	int best_temp_diff = INT_MAX, temp_diff;
	u8 i, best_index = 0;

	if (!info->ocv_table[0])
		return NULL;

	for (i = 0; i < POWER_SUPPLY_OCV_TEMP_MAX; i++) {
		/* Out of capacity tables */
		if (!info->ocv_table[i])
			break;

		temp_diff = abs(info->ocv_temp[i] - temp);

		if (temp_diff < best_temp_diff) {
			best_temp_diff = temp_diff;
			best_index = i;
		}
	}

	*table_len = info->ocv_table_size[best_index];
	return info->ocv_table[best_index];
}
EXPORT_SYMBOL_GPL(power_supply_find_ocv2cap_table);

int power_supply_batinfo_ocv2cap(struct power_supply_battery_info *info,
				 int ocv, int temp)
{
	struct power_supply_battery_ocv_table *table;
	int table_len;

	table = power_supply_find_ocv2cap_table(info, temp, &table_len);
	if (!table)
		return -EINVAL;

	return power_supply_ocv2cap_simple(table, table_len, ocv);
}
EXPORT_SYMBOL_GPL(power_supply_batinfo_ocv2cap);

static bool chihiro_is_sdp_connected(struct power_supply *caller_psy)
{
	union power_supply_propval uval, oval;

	if (caller_psy && caller_psy->desc && caller_psy->desc->get_property) {
		if (caller_psy->desc->get_property(caller_psy, POWER_SUPPLY_PROP_ONLINE, &oval) == 0 &&
		    oval.intval == 0)
			return false;
		if (caller_psy->desc->get_property(caller_psy, POWER_SUPPLY_PROP_USB_TYPE, &uval) == 0) {
			if (uval.intval == POWER_SUPPLY_USB_TYPE_SDP ||
			    uval.intval == POWER_SUPPLY_USB_TYPE_CDP)
				return true;
		}
	}

	{
		struct power_supply *chg = power_supply_get_by_name("charger");
		if (chg) {
			int ret = -1;
			if (chg->desc && chg->desc->get_property) {
				if (chg->desc->get_property(chg, POWER_SUPPLY_PROP_ONLINE, &oval) == 0 &&
				    oval.intval == 0) {
					power_supply_put(chg);
					return false;
				}
				ret = chg->desc->get_property(chg, POWER_SUPPLY_PROP_USB_TYPE, &uval);
			}
			power_supply_put(chg);
			if (ret == 0 && (uval.intval == POWER_SUPPLY_USB_TYPE_SDP ||
					 uval.intval == POWER_SUPPLY_USB_TYPE_CDP))
				return true;
		}
	}
	return false;
}

int chihiro_charge_limit_ua = 0;
EXPORT_SYMBOL_GPL(chihiro_charge_limit_ua);

static bool chihiro_bypass_active = false;
static bool chihiro_user_mode_locked = false;
static int *chihiro_memtest_cur_ptr = NULL;
static int *chihiro_kick_to_80_ptr = NULL;
static int *chihiro_monkey_flag_ptr = NULL;
static int *chihiro_max_temp_delta_ptr = NULL;
static int *chihiro_ibat_temp_cur_ptr = NULL;

static void chihiro_sync_mtk_charger_info(void *info, bool wake_charger, struct power_supply *mchg_psy)
{
	u32 bypass_chg;
	u8 cmd_dischg;
	int i, k;

	if (!info)
		return;

	if (!chihiro_memtest_cur_ptr)
		chihiro_memtest_cur_ptr = (int *)kallsyms_lookup_name("memtest_input_current");
	if (!chihiro_kick_to_80_ptr)
		chihiro_kick_to_80_ptr = (int *)kallsyms_lookup_name("kick_to_80");
	if (!chihiro_monkey_flag_ptr)
		chihiro_monkey_flag_ptr = (int *)kallsyms_lookup_name("monkey_flag");
	if (!chihiro_max_temp_delta_ptr)
		chihiro_max_temp_delta_ptr = (int *)kallsyms_lookup_name("g_max_temp_delta");
	if (!chihiro_ibat_temp_cur_ptr)
		chihiro_ibat_temp_cur_ptr = (int *)kallsyms_lookup_name("g_ibat_temp_cur");

	bypass_chg = *(u32 *)((char *)info + 144);
	cmd_dischg = *(u8 *)((char *)info + 2436);

	if (chihiro_bypass_active ||
	    (!chihiro_user_mode_locked && (bypass_chg == 1 || cmd_dischg == 1))) {
		if (chihiro_memtest_cur_ptr)
			*chihiro_memtest_cur_ptr = 0;

		*(u8 *)((char *)info + 0xc80) = 0; /* Disable HV pumps during bypass */

		for (i = 0; i < 10; i++) {
			void *alg = *(void **)((char *)info + 0xc18 + i * 8);
			u32 alg_id;
			if (!alg)
				continue;
			alg_id = *(u32 *)((char *)alg + 0x6cc);
			if (alg_id == 0x10) {
				void *pe5_drv = *(void **)((char *)alg + 0x6b8);
				void *pe5_info = pe5_drv ? *(void **)((char *)pe5_drv + 0x18) : NULL;
				if (pe5_info) {
					*(u32 *)((char *)pe5_info + 0x33c) = 1;
					if (*(u8 *)((char *)pe5_info + 0x14c) != 0 &&
					    *(u32 *)((char *)pe5_info + 0x2e8) != 5) {
						smp_store_release((u32 *)((char *)pe5_info + 0x110), 1);
						smp_store_release((u32 *)((char *)pe5_info + 0x108), 1);
						wake_up((wait_queue_head_t *)((char *)pe5_info + 0xf0));
					}
				}
			}
		}

		if (wake_charger) {
			*(u8 *)((char *)info + 0x590) = 1;
			wake_up((wait_queue_head_t *)((char *)info + 0x578));
		}
		return;
	}

	/* Ensure charger is not stuck in bypass/discharge/80%-cutoff state */
	*(u32 *)((char *)info + 136) = 0;   /* tran_aichg_disable_charger = 0 */
	*(u32 *)((char *)info + 144) = 0;   /* bypass_charger = 0 */
	*(u32 *)((char *)info + 148) = 100; /* bypass_energy = 100 */
	*(u8 *)((char *)info + 2436) = 0;   /* cmd_discharging = 0 */
	*(u8 *)((char *)info + 0x98d) = 0;
	if (chihiro_kick_to_80_ptr)
		*chihiro_kick_to_80_ptr = 0;
	if (chihiro_monkey_flag_ptr)
		*chihiro_monkey_flag_ptr = 0;

	if (chihiro_charge_limit_ua == 1000000 || chihiro_charge_limit_ua == 2000000) {
		int limit_ma = (chihiro_charge_limit_ua == 1000000) ? 1000 : 2000;

		if (chihiro_memtest_cur_ptr)
			*chihiro_memtest_cur_ptr = limit_ma;

		/* Disable HV charge pumps (pe50/tc30) and force 5V buck charger at 5W/10W */
		*(u8 *)((char *)info + 0xc80) = 0; /* enable_hv_charging = 0 */
		*(u8 *)((char *)info + 0xf1e) = 0; /* allow swchg chg1_dev */
		*(int *)((char *)info + 272) = chihiro_charge_limit_ua;
		*(int *)((char *)info + 276) = chihiro_charge_limit_ua;
		*(int *)((char *)info + 284) = chihiro_charge_limit_ua;
		*(int *)((char *)info + 288) = chihiro_charge_limit_ua;
		*(int *)((char *)info + 300) = -1;
		*(int *)((char *)info + 452) = chihiro_charge_limit_ua;
		*(int *)((char *)info + 456) = chihiro_charge_limit_ua;
		*(int *)((char *)info + 2836) = chihiro_charge_limit_ua;
		*(int *)((char *)info + 2840) = chihiro_charge_limit_ua;
		*(int *)((char *)info + 0xd8c) = chihiro_charge_limit_ua;

		for (i = 0; i < 10; i++) {
			void *alg = *(void **)((char *)info + 0xc18 + i * 8);
			u32 alg_id;
			if (!alg)
				continue;
			alg_id = *(u32 *)((char *)alg + 0x6cc);
			if (alg_id == 0x10) {
				void *pe5_drv = *(void **)((char *)alg + 0x6b8);
				void *pe5_info = pe5_drv ? *(void **)((char *)pe5_drv + 0x18) : NULL;
				if (pe5_info) {
					*(u32 *)((char *)pe5_info + 0x33c) = 1;
					*(int *)((char *)pe5_info + 0x304) = limit_ma;
					*(int *)((char *)pe5_info + 0x308) = limit_ma;
					if (*(u8 *)((char *)pe5_info + 0x14c) != 0 &&
					    *(u32 *)((char *)pe5_info + 0x2e8) != 5) {
						smp_store_release((u32 *)((char *)pe5_info + 0x110), 1);
						smp_store_release((u32 *)((char *)pe5_info + 0x108), 1);
						wake_up((wait_queue_head_t *)((char *)pe5_info + 0xf0));
					}
				}
			}
		}
	} else {
		/* FAST 68W SuperCharge: Clear memtest_input_current, thermal & 80% SoC limits */
		if (chihiro_memtest_cur_ptr)
			*chihiro_memtest_cur_ptr = 0;
		if (chihiro_max_temp_delta_ptr)
			*chihiro_max_temp_delta_ptr = 0;
		if (chihiro_ibat_temp_cur_ptr &&
		    *chihiro_ibat_temp_cur_ptr > 0 &&
		    *chihiro_ibat_temp_cur_ptr < 6000000)
			*chihiro_ibat_temp_cur_ptr = 6000000;

		*(u8 *)((char *)info + 0xc80) = 1; /* enable_hv_charging = 1 */
		*(u8 *)((char *)info + 0xd68) = 0; /* Disable SmartCharging 80% cap */
		*(int *)((char *)info + 0xd70) = -1;
		*(int *)((char *)info + 284) = -1;
		*(int *)((char *)info + 288) = -1;
		*(int *)((char *)info + 300) = -1;
		*(int *)((char *)info + 328) = -1;
		*(int *)((char *)info + 332) = -1;
		*(int *)((char *)info + 372) = -1;
		*(int *)((char *)info + 0xd8c) = -1;
		*(int *)((char *)info + 272) = 3400000;
		*(int *)((char *)info + 276) = 6000000;
		*(int *)((char *)info + 2836) = 6000000;
		*(int *)((char *)info + 2840) = 3400000;

		for (i = 0; i < 10; i++) {
			void *alg = *(void **)((char *)info + 0xc18 + i * 8);
			u32 alg_id;
			if (!alg)
				continue;
			alg_id = *(u32 *)((char *)alg + 0x6cc);
			if (alg_id == 0x10) {
				void *pe5_drv = *(void **)((char *)alg + 0x6b8);
				void *pe5_desc = pe5_drv ? *(void **)((char *)pe5_drv + 0x10) : NULL;
				void *pe5_info = pe5_drv ? *(void **)((char *)pe5_drv + 0x18) : NULL;
				if (pe5_info) {
					u32 base_ita = *(u32 *)((char *)pe5_info + 0x2ac);
					*(u8 *)((char *)pe5_info + 0x14c) = 1; /* inited = 1 */
					*(u32 *)((char *)pe5_info + 0x110) = 0;
					*(u32 *)((char *)pe5_info + 0x33c) = 0;
					*(int *)((char *)pe5_info + 0x304) = -1;
					*(int *)((char *)pe5_info + 0x308) = -1;
					*(u32 *)((char *)pe5_info + 0x314) = 0;
					*(u8 *)((char *)pe5_info + 0x330) = 0;
					*(int *)((char *)pe5_info + 0x414) = -1;
					if (base_ita > 0)
						*(u32 *)((char *)pe5_info + 0x2b0) = base_ita;
					/* Keep PE5.0 thermal state machine in NORMAL (4), valid range 1..7 */
					*(u32 *)((char *)pe5_info + 0x2ec) = 4;
					*(u32 *)((char *)pe5_info + 0x2f0) = 4;
					*(u32 *)((char *)pe5_info + 0x2f4) = 4;
					*(u32 *)((char *)pe5_info + 0x2f8) = 4;
					*(u32 *)((char *)pe5_info + 0x2fc) = 4;
					*(u32 *)((char *)pe5_info + 0x300) = 4;
					*(u32 *)((char *)pe5_info + 0x3e4) = 4;
					if (wake_charger) {
						*(u8 *)((char *)pe5_info + 0x14e) = 0;
						*(u32 *)((char *)pe5_info + 0x148) = 0;
						if (*(u32 *)((char *)pe5_info + 0x2e8) != 5) {
							smp_store_release((u32 *)((char *)pe5_info + 0x108), 1);
							wake_up((wait_queue_head_t *)((char *)pe5_info + 0xf0));
						}
					}
				}
				if (pe5_desc) {
					*(int *)((char *)pe5_desc + 0x1c) = 100;
					*(u32 *)((char *)pe5_desc + 0xbd0) = 6200;
					*(u32 *)((char *)pe5_desc + 0xbd8) = 6200;
					for (k = 0; k < 8; k++)
						*(u32 *)((char *)pe5_desc + 0xe8 + k * 4) = 0;
					for (k = 0; k < 9; k++) {
						*(u32 *)((char *)pe5_desc + 0x954 + k * 4) = 0;
						*(u32 *)((char *)pe5_desc + 0x99c + k * 4) = 0;
						*(u32 *)((char *)pe5_desc + 0x9e4 + k * 4) = 0;
						*(u32 *)((char *)pe5_desc + 0xa2c + k * 4) = 0;
						*(u32 *)((char *)pe5_desc + 0xa74 + k * 4) = 0;
						*(u32 *)((char *)pe5_desc + 0xabc + k * 4) = 0;
						*(u32 *)((char *)pe5_desc + 0xb4c + k * 4) = 0;
						*(u32 *)((char *)pe5_desc + 0xc18 + k * 4) = 0;
						*(u32 *)((char *)pe5_desc + 0xc60 + k * 4) = 0;
					}
				}
			} else if (alg_id == 0x4) {
				void *tc30_info = *(void **)((char *)alg + 0xe0);
				if (tc30_info && *(int *)((char *)tc30_info + 0xe0) >= 80 &&
				    *(int *)((char *)tc30_info + 0xe0) < 100)
					*(int *)((char *)tc30_info + 0xe0) = 100;
			}
		}
	}

	if (wake_charger) {
		*(u8 *)((char *)info + 0x590) = 1;
		wake_up((wait_queue_head_t *)((char *)info + 0x578));
	}
}

static void chihiro_apply_hardware_limits(void)
{
	static bool in_apply = false;
	struct power_supply *mchg = NULL;

	if (in_apply)
		return;
	in_apply = true;

	mchg = power_supply_get_by_name("mtk-master-charger");
	if (mchg) {
		void *info = power_supply_get_drvdata(mchg);
		if (info)
			chihiro_sync_mtk_charger_info(info, true, mchg);
		power_supply_put(mchg);
	}

	in_apply = false;
}

static struct delayed_work chihiro_rfc_reset_work;
static struct delayed_work chihiro_usb_unplug_work;
static bool chihiro_rfc_work_ready = false;
static bool chihiro_has_reset_rfc = false;
static unsigned long chihiro_last_rfc_reset_jiffies = 0;
static unsigned long chihiro_last_unplug_reset_jiffies = 0;
static int chihiro_auto_rfc_retries = 0;

static noinline __nocfi void chihiro_call_dcp_reset_rfc(void *fn)
{
	int (*reset_fn)(void) = (int (*)(void))fn;

	if (reset_fn)
		reset_fn();
}

static noinline __nocfi int chihiro_call_int_fn(void *fn)
{
	int (*int_fn)(void) = (int (*)(void))fn;

	return int_fn ? int_fn() : -1;
}

static bool chihiro_is_charger_online(void)
{
	struct power_supply *chg = power_supply_get_by_name("charger");
	union power_supply_propval oval;
	bool online = false;

	if (chg) {
		if (chg->desc && chg->desc->get_property &&
		    chg->desc->get_property(chg, POWER_SUPPLY_PROP_ONLINE, &oval) == 0 &&
		    oval.intval > 0)
			online = true;
		power_supply_put(chg);
	}
	return online;
}

static bool chihiro_is_otg_active(void)
{
	static void *otg_plug_fn;

	if (!otg_plug_fn)
		otg_plug_fn = (void *)kallsyms_lookup_name("tran_get_otg_plug_in");
	return otg_plug_fn && chihiro_call_int_fn(otg_plug_fn) > 0;
}

static void chihiro_usb_unplug_workfn(struct work_struct *work)
{
	struct power_supply *sw_psy;
	void *sw_dev;
	void *reset_fn;

	if (chihiro_is_charger_online() && !chihiro_is_otg_active())
		return;

	if (chihiro_rfc_work_ready)
		cancel_delayed_work(&chihiro_rfc_reset_work);

	reset_fn = (void *)kallsyms_lookup_name("dcp_reset_rfc");
	if (reset_fn)
		chihiro_call_dcp_reset_rfc(reset_fn);

	sw_psy = power_supply_get_by_name("usb_switch");
	if (sw_psy) {
		sw_dev = power_supply_get_drvdata(sw_psy);
		if (sw_dev) {
			/* Clear stale RFC/charger state so D+/D- returns to MTK PHY */
			*(u8 *)((char *)sw_dev + 0x100) = 0;
			*(u8 *)((char *)sw_dev + 0x178) = 0;
			*(u8 *)((char *)sw_dev + 0x17a) = 0;
			*(u32 *)((char *)sw_dev + 0x188) = 0;
		}
		power_supply_put(sw_psy);
	}
}

static void chihiro_maybe_reset_usb_switch(void)
{
	struct power_supply *sw_psy;
	void *sw_dev;
	bool need_reset = false;

	if (!chihiro_rfc_work_ready)
		return;
	if (chihiro_is_charger_online() && !chihiro_is_otg_active())
		return;
	if (chihiro_last_unplug_reset_jiffies != 0 &&
	    time_before(jiffies, chihiro_last_unplug_reset_jiffies + msecs_to_jiffies(500)))
		return;

	sw_psy = power_supply_get_by_name("usb_switch");
	if (sw_psy) {
		sw_dev = power_supply_get_drvdata(sw_psy);
		if (sw_dev) {
			u8 chg_in = *(u8 *)((char *)sw_dev + 0x100);
			u8 rfc_dcp_ta = *(u8 *)((char *)sw_dev + 0x178);
			u32 rfc_online = *(u32 *)((char *)sw_dev + 0x188);

			if (chg_in || rfc_dcp_ta || rfc_online)
				need_reset = true;
		}
		power_supply_put(sw_psy);
	}

	if (need_reset) {
		chihiro_last_unplug_reset_jiffies = jiffies;
		schedule_delayed_work(&chihiro_usb_unplug_work, 0);
	}
}

static void *chihiro_find_pe5_info(void *info)
{
	int i;

	if (!info)
		return NULL;
	for (i = 0; i < 10; i++) {
		void *alg = *(void **)((char *)info + 0xc18 + i * 8);
		if (alg && *(u32 *)((char *)alg + 0x6cc) == 0x10) {
			void *pe5_drv = *(void **)((char *)alg + 0x6b8);
			return pe5_drv ? *(void **)((char *)pe5_drv + 0x18) : NULL;
		}
	}
	return NULL;
}

/*
 * Re-trigger Infinix 68W RFC handshake in usb_switch when PE5.0 stopped
 * (e.g. after Slow/Normal/Bypass hardreset or rfc_ta 7-001b -ENXIO) without
 * requiring a physical USB cable unplug.
 */
static void chihiro_rfc_reset_workfn(struct work_struct *work)
{
	struct power_supply *mchg = NULL;
	struct power_supply *sw_psy = NULL;
	void *info = NULL;
	void *sw_dev = NULL;
	void *pe5_info = NULL;
	bool pe5_running = false;
	bool need_rehandshake = false;
	bool rfc_busy = false;
	int wait_loops;

	if (chihiro_bypass_active || chihiro_charge_limit_ua != 0)
		return;
	if (!chihiro_is_charger_online())
		return;

	mchg = power_supply_get_by_name("mtk-master-charger");
	if (!mchg)
		return;

	info = power_supply_get_drvdata(mchg);
	pe5_info = chihiro_find_pe5_info(info);
	if (pe5_info) {
		u32 state = *(u32 *)((char *)pe5_info + 0x2e8);
		*(u8 *)((char *)pe5_info + 0x14c) = 1; /* inited must stay 1 */
		if (state != 5)
			pe5_running = true;
	}
	power_supply_put(mchg);

	if (pe5_running)
		return;

	sw_psy = power_supply_get_by_name("usb_switch");
	if (sw_psy) {
		sw_dev = power_supply_get_drvdata(sw_psy);
		if (sw_dev) {
			bool is_pd = (*(u8 *)((char *)sw_dev + 0x17a) != 0);
			bool chg_in = (*(u8 *)((char *)sw_dev + 0x100) != 0);
			struct work_struct *rfc_work =
				(struct work_struct *)((char *)sw_dev + 0x8);

			rfc_busy = (work_busy(rfc_work) != 0);
			if (!is_pd && chg_in && !rfc_busy)
				need_rehandshake = true;
		}
		power_supply_put(sw_psy);
	}

	/* If usb_switch is already running plug-in detection, wait for it instead of resetting */
	if (rfc_busy) {
		for (wait_loops = 0; wait_loops < 12; wait_loops++) {
			bool busy = false;
			msleep(50);
			sw_psy = power_supply_get_by_name("usb_switch");
			if (!sw_psy)
				break;
			sw_dev = power_supply_get_drvdata(sw_psy);
			if (sw_dev) {
				struct work_struct *rfc_work =
					(struct work_struct *)((char *)sw_dev + 0x8);
				busy = (work_busy(rfc_work) != 0);
			}
			power_supply_put(sw_psy);
			if (!busy)
				break;
		}
	} else if (need_rehandshake) {
		void *reset_sym = (void *)kallsyms_lookup_name("dcp_reset_rfc");
		if (reset_sym)
			chihiro_call_dcp_reset_rfc(reset_sym);

		msleep(200);

		if (chihiro_bypass_active || chihiro_charge_limit_ua != 0 ||
		    !chihiro_is_charger_online())
			return;

		sw_psy = power_supply_get_by_name("usb_switch");
		if (sw_psy) {
			sw_dev = power_supply_get_drvdata(sw_psy);
			if (sw_dev && *(u8 *)((char *)sw_dev + 0x17a) == 0) {
				struct work_struct *rfc_work =
					(struct work_struct *)((char *)sw_dev + 0x8);
				*(u8 *)((char *)sw_dev + 0x100) = 0; /* chg_in = 0 */
				*(u8 *)((char *)sw_dev + 0x178) = 0; /* rfc_dcp_ta = 0 */
				queue_work(system_wq, rfc_work);
			}
			power_supply_put(sw_psy);
		}

		for (wait_loops = 0; wait_loops < 12; wait_loops++) {
			bool busy = false;
			msleep(50);
			if (chihiro_bypass_active || chihiro_charge_limit_ua != 0 ||
			    !chihiro_is_charger_online())
				return;
			sw_psy = power_supply_get_by_name("usb_switch");
			if (!sw_psy)
				break;
			sw_dev = power_supply_get_drvdata(sw_psy);
			if (sw_dev) {
				struct work_struct *rfc_work =
					(struct work_struct *)((char *)sw_dev + 0x8);
				busy = (work_busy(rfc_work) != 0);
			}
			power_supply_put(sw_psy);
			if (!busy && wait_loops >= 2)
				break;
		}
	}

	mchg = power_supply_get_by_name("mtk-master-charger");
	if (mchg) {
		info = power_supply_get_drvdata(mchg);
		pe5_info = chihiro_find_pe5_info(info);
		if (pe5_info) {
			u32 state = *(u32 *)((char *)pe5_info + 0x2e8);
			*(u8 *)((char *)pe5_info + 0x14c) = 1; /* inited = 1 (NEVER 0!) */
			if (state == 5) {
				*(u8 *)((char *)pe5_info + 0x14d) = 0; /* ta_ready = 0 -> re-auth */
				*(u8 *)((char *)pe5_info + 0x14e) = 0; /* run_once = 0 */
				*(u32 *)((char *)pe5_info + 0x148) = 0;
				*(u32 *)((char *)pe5_info + 0x3ec) = 1; /* rfc_ta_det_done = 1 */
				*(u32 *)((char *)pe5_info + 0x33c) = 0; /* pe50_ai_discharger = 0 */
			}
		}
		if (info)
			chihiro_sync_mtk_charger_info(info, true, mchg);
		power_supply_put(mchg);
	}
}

void chihiro_charging_sysfs_write(const char *name, const char *buf)
{
	long val = 0;
	bool matched = false;
	bool prev_bypass = chihiro_bypass_active;
	int prev_limit_ua = chihiro_charge_limit_ua;
	bool request_renegotiate = false;

	if (!name || !buf)
		return;

	if (!strcmp(name, "bypass_charger") ||
	    !strcmp(name, "cmd_discharging") ||
	    !strcmp(name, "tran_aichg_disable_charger") ||
	    !strcmp(name, "High_voltage_chg_enable") ||
	    !strcmp(name, "input_current") ||
	    !strcmp(name, "sc_ibat_limit") ||
	    !strcmp(name, "adapter_capacity") ||
	    !strcmp(name, "enable_sc") ||
	    !strcmp(name, "limit_current") ||
	    !strcmp(name, "chihiro_charge_limit")) {
		matched = true;
	}

	if (!matched)
		return;

	if (kstrtol(buf, 10, &val) != 0) {
		if (kstrtol(buf, 16, &val) != 0)
			return;
	}

	if (!strcmp(name, "chihiro_charge_limit")) {
		chihiro_user_mode_locked = true;
		if (val == 99) {
			chihiro_bypass_active = true;
		} else {
			chihiro_bypass_active = false;
			if (val == 5 || val == 1000 || val == 5000 || val == 1000000 || (val > 0 && val <= 1200))
				chihiro_charge_limit_ua = 1000000; /* SLOW 5W */
			else if (val == 10 || val == 12 || val == 2000 || val == 10000 || val == 2000000 || (val > 1200 && val <= 2500))
				chihiro_charge_limit_ua = 2000000; /* NORMAL 10W */
			else if (val <= 0 || val == 44 || val == 68 || val >= 3000) {
				chihiro_charge_limit_ua = 0;       /* FAST 68W */
				if (val == 68 || prev_bypass || prev_limit_ua > 0)
					request_renegotiate = true;
			}
		}
	} else if (!strcmp(name, "bypass_charger") ||
		   !strcmp(name, "cmd_discharging") ||
		   !strcmp(name, "tran_aichg_disable_charger")) {
		chihiro_bypass_active = (val > 0);
		if (!chihiro_bypass_active && prev_bypass && chihiro_charge_limit_ua == 0)
			request_renegotiate = true;
	} else if (!chihiro_user_mode_locked) {
		/* Ignore SmartCharging/XOS thermal daemon writes unless user hasn't locked mode yet;
		 * never treat SmartCharging enable_sc or mA sc_ibat_limit/limit_current as mode downgrades.
		 */
		if (!strcmp(name, "High_voltage_chg_enable") && val == 1) {
			chihiro_bypass_active = false;
			chihiro_charge_limit_ua = 0;
			if (prev_bypass || prev_limit_ua > 0)
				request_renegotiate = true;
		}
	}

	chihiro_apply_hardware_limits();

	if (request_renegotiate && chihiro_rfc_work_ready &&
	    !chihiro_bypass_active && chihiro_charge_limit_ua == 0 &&
	    (!chihiro_has_reset_rfc ||
	     time_after(jiffies, chihiro_last_rfc_reset_jiffies + msecs_to_jiffies(2500)))) {
		chihiro_has_reset_rfc = true;
		chihiro_auto_rfc_retries = 0;
		chihiro_last_rfc_reset_jiffies = jiffies;
		schedule_delayed_work(&chihiro_rfc_reset_work, msecs_to_jiffies(100));
	}
}
EXPORT_SYMBOL_GPL(chihiro_charging_sysfs_write);

static ssize_t chihiro_charge_limit_proc_read(struct file *file, char __user *ubuf,
					      size_t count, loff_t *ppos)
{
	char buf[32];
	int len = scnprintf(buf, sizeof(buf), "%d\n", chihiro_charge_limit_ua);
	return simple_read_from_buffer(ubuf, count, ppos, buf, len);
}

static ssize_t chihiro_charge_limit_proc_write(struct file *file, const char __user *ubuf,
					       size_t count, loff_t *ppos)
{
	char kbuf[32];
	size_t len;

	if (!ubuf || count == 0)
		return -EINVAL;

	len = min(count, sizeof(kbuf) - 1);
	if (copy_from_user(kbuf, ubuf, len))
		return -EFAULT;
	kbuf[len] = '\0';

	chihiro_charging_sysfs_write("chihiro_charge_limit", kbuf);
	return count;
}

static const struct proc_ops chihiro_charge_limit_proc_ops = {
	.proc_read = chihiro_charge_limit_proc_read,
	.proc_write = chihiro_charge_limit_proc_write,
	.proc_lseek = default_llseek,
};

static void chihiro_init_proc_entry(void)
{
	struct proc_dir_entry *entry;

	INIT_DELAYED_WORK(&chihiro_rfc_reset_work, chihiro_rfc_reset_workfn);
	INIT_DELAYED_WORK(&chihiro_usb_unplug_work, chihiro_usb_unplug_workfn);
	chihiro_rfc_work_ready = true;

	entry = proc_create("chihiro_charge_limit", 0666, NULL, &chihiro_charge_limit_proc_ops);
	if (entry)
		proc_set_user(entry, GLOBAL_ROOT_UID, GLOBAL_ROOT_GID);
}

int power_supply_get_property(struct power_supply *psy,
			    enum power_supply_property psp,
			    union power_supply_propval *val)
{
	int ret;

	if (atomic_read(&psy->use_cnt) <= 0) {
		if (!psy->initialized)
			return -EAGAIN;
		return -ENODEV;
	}

	ret = psy->desc->get_property(psy, psp, val);
	if (ret < 0)
		return ret;

	if (psy->desc && psy->desc->name) {
		const char *pname = psy->desc->name;
		if (!strcmp(pname, "mtk-mst-div-chg") ||
		    !strcmp(pname, "mtk-slv-div-chg") ||
		    !strcmp(pname, "primary_dvchg") ||
		    !strcmp(pname, "secondary_dvchg") ||
		    !strcmp(pname, "divider_charger")) {
			if (chihiro_charge_limit_ua > 0) {
				if (psp == POWER_SUPPLY_PROP_ONLINE ||
				    psp == POWER_SUPPLY_PROP_PRESENT) {
					val->intval = 0;
					return 0;
				} else if (psp == POWER_SUPPLY_PROP_CURRENT_MAX ||
					   psp == POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT ||
					   psp == POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT ||
					   psp == POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX) {
					val->intval = 0;
					return 0;
				}
			}
		} else if (!strcmp(pname, "usb_switch")) {
			if (psp == POWER_SUPPLY_PROP_ONLINE &&
			    (!chihiro_is_charger_online() || chihiro_is_otg_active())) {
				void *sw_dev = power_supply_get_drvdata(psy);

				val->intval = 0;
				if (sw_dev)
					*(u32 *)((char *)sw_dev + 0x188) = 0;
				chihiro_maybe_reset_usb_switch();
			}
		} else if (!strcmp(pname, "charger") ||
			   !strcmp(pname, "mtk-master-charger") ||
			   !strcmp(pname, "primary_chg") ||
			   !strcmp(pname, "rt9471_chg")) {
			union power_supply_propval oval;
			bool is_online = false;

			if (psy->desc->get_property(psy, POWER_SUPPLY_PROP_ONLINE, &oval) == 0 &&
			    oval.intval > 0)
				is_online = true;

			if (!strcmp(pname, "mtk-master-charger") &&
			    (is_online || chihiro_bypass_active)) {
				void *info = power_supply_get_drvdata(psy);
				if (info)
					chihiro_sync_mtk_charger_info(info, false, NULL);
			}

			if (!is_online || chihiro_is_otg_active()) {
				if (chihiro_rfc_work_ready)
					cancel_delayed_work(&chihiro_rfc_reset_work);
				if (!strcmp(pname, "mtk-master-charger") ||
				    !strcmp(pname, "charger"))
					chihiro_maybe_reset_usb_switch();
			}

			if (!strcmp(pname, "mtk-master-charger")) {
				if (!is_online || chihiro_is_otg_active()) {
					chihiro_auto_rfc_retries = 0;
					chihiro_last_rfc_reset_jiffies = jiffies;
				} else if (!chihiro_bypass_active && chihiro_charge_limit_ua == 0 &&
					   chihiro_rfc_work_ready) {
					void *info = power_supply_get_drvdata(psy);
					void *pe5_info = chihiro_find_pe5_info(info);
					if (chihiro_last_rfc_reset_jiffies == 0)
						chihiro_last_rfc_reset_jiffies = jiffies;
					if (pe5_info) {
						u32 state = *(u32 *)((char *)pe5_info + 0x2e8);
						if (state != 5) {
							chihiro_auto_rfc_retries = 0;
							chihiro_last_rfc_reset_jiffies = jiffies;
						} else if (chihiro_auto_rfc_retries < 3 &&
							   time_after(jiffies, chihiro_last_rfc_reset_jiffies + msecs_to_jiffies(6000))) {
							chihiro_has_reset_rfc = true;
							chihiro_auto_rfc_retries++;
							chihiro_last_rfc_reset_jiffies = jiffies;
							schedule_delayed_work(&chihiro_rfc_reset_work, msecs_to_jiffies(200));
						}
					}
				}
			}

			if (!is_online &&
			    (psp == POWER_SUPPLY_PROP_TYPE ||
			     psp == POWER_SUPPLY_PROP_USB_TYPE)) {
				val->intval = 0;
			} else if (psp == POWER_SUPPLY_PROP_TYPE) {
				if (is_online) {
					if (chihiro_is_sdp_connected(psy))
						val->intval = POWER_SUPPLY_TYPE_USB;
					else if (val->intval == POWER_SUPPLY_TYPE_UNKNOWN)
						val->intval = POWER_SUPPLY_TYPE_USB_DCP;
				}
			} else if (psp == POWER_SUPPLY_PROP_CURRENT_MAX ||
				   psp == POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT ||
				   psp == POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT ||
				   psp == POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX) {
				if (chihiro_charge_limit_ua == 1000000) {
					val->intval = 1000000;
				} else if (chihiro_charge_limit_ua == 2000000) {
					val->intval = 2000000;
				} else if (is_online && !chihiro_is_sdp_connected(psy)) {
					if (psp == POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT ||
					    psp == POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX) {
						if (val->intval < 6000000)
							val->intval = 6000000;
					} else if (val->intval < 3400000) {
						val->intval = 3400000;
					}
				}
			} else if (psp == POWER_SUPPLY_PROP_INPUT_VOLTAGE_LIMIT ||
				   psp == POWER_SUPPLY_PROP_VOLTAGE_MAX) {
				if (chihiro_charge_limit_ua > 0 && val->intval > 5000000) {
					val->intval = 5000000;
				}
			}
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(power_supply_get_property);

int power_supply_set_property(struct power_supply *psy,
			    enum power_supply_property psp,
			    const union power_supply_propval *val)
{
	union power_supply_propval modified_val;

	if (atomic_read(&psy->use_cnt) <= 0 || !psy->desc->set_property)
		return -ENODEV;

	if (val) {
		modified_val = *val;
		if (psy->desc && psy->desc->name) {
			const char *pname = psy->desc->name;
			if (!strcmp(pname, "mtk-mst-div-chg") ||
			    !strcmp(pname, "mtk-slv-div-chg") ||
			    !strcmp(pname, "primary_dvchg") ||
			    !strcmp(pname, "secondary_dvchg") ||
			    !strcmp(pname, "divider_charger")) {
				if (chihiro_charge_limit_ua > 0) {
					if (psp == POWER_SUPPLY_PROP_ONLINE ||
					    psp == POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT ||
					    psp == POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT ||
					    psp == POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX ||
					    psp == POWER_SUPPLY_PROP_CURRENT_MAX) {
						modified_val.intval = 0;
					}
				}
			} else if (!strcmp(pname, "charger") ||
				   !strcmp(pname, "mtk-master-charger") ||
				   !strcmp(pname, "primary_chg") ||
				   !strcmp(pname, "rt9471_chg")) {
				if (psp == POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT ||
				    psp == POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT ||
				    psp == POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX ||
				    psp == POWER_SUPPLY_PROP_CURRENT_MAX) {
					if (chihiro_charge_limit_ua == 1000000) {
						if (modified_val.intval > 1000000 || modified_val.intval <= 0)
							modified_val.intval = 1000000;
					} else if (chihiro_charge_limit_ua == 2000000) {
						if (modified_val.intval > 2000000 || modified_val.intval <= 0)
							modified_val.intval = 2000000;
					}
				} else if (psp == POWER_SUPPLY_PROP_INPUT_VOLTAGE_LIMIT ||
					   psp == POWER_SUPPLY_PROP_VOLTAGE_MAX) {
					if (chihiro_charge_limit_ua > 0 && modified_val.intval > 5000000) {
						modified_val.intval = 5000000;
					}
				}
			}
		}
		{
			int ret = psy->desc->set_property(psy, psp, &modified_val);
			if (psy->desc && psy->desc->name &&
			    !strcmp(psy->desc->name, "mtk-master-charger")) {
				if (chihiro_is_charger_online() || chihiro_bypass_active) {
					void *info = power_supply_get_drvdata(psy);
					if (info)
						chihiro_sync_mtk_charger_info(info, false, NULL);
				} else {
					chihiro_maybe_reset_usb_switch();
				}
			}
			return ret;
		}
	}

	return psy->desc->set_property(psy, psp, val);
}
EXPORT_SYMBOL_GPL(power_supply_set_property);

int power_supply_property_is_writeable(struct power_supply *psy,
					enum power_supply_property psp)
{
	if (atomic_read(&psy->use_cnt) <= 0 ||
			!psy->desc->property_is_writeable)
		return -ENODEV;

	return psy->desc->property_is_writeable(psy, psp);
}
EXPORT_SYMBOL_GPL(power_supply_property_is_writeable);

void power_supply_external_power_changed(struct power_supply *psy)
{
	if (atomic_read(&psy->use_cnt) <= 0 ||
			!psy->desc->external_power_changed)
		return;

	psy->desc->external_power_changed(psy);
}
EXPORT_SYMBOL_GPL(power_supply_external_power_changed);

int power_supply_powers(struct power_supply *psy, struct device *dev)
{
	return sysfs_create_link(&psy->dev.kobj, &dev->kobj, "powers");
}
EXPORT_SYMBOL_GPL(power_supply_powers);

static void power_supply_dev_release(struct device *dev)
{
	struct power_supply *psy = to_power_supply(dev);
	dev_dbg(dev, "%s\n", __func__);
	kfree(psy);
}

int power_supply_reg_notifier(struct notifier_block *nb)
{
	return atomic_notifier_chain_register(&power_supply_notifier, nb);
}
EXPORT_SYMBOL_GPL(power_supply_reg_notifier);

void power_supply_unreg_notifier(struct notifier_block *nb)
{
	atomic_notifier_chain_unregister(&power_supply_notifier, nb);
}
EXPORT_SYMBOL_GPL(power_supply_unreg_notifier);

#ifdef CONFIG_THERMAL
static int power_supply_read_temp(struct thermal_zone_device *tzd,
		int *temp)
{
	struct power_supply *psy;
	union power_supply_propval val;
	int ret;

	WARN_ON(tzd == NULL);
	psy = tzd->devdata;
	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_TEMP, &val);
	if (ret)
		return ret;

	/* Convert tenths of degree Celsius to milli degree Celsius. */
	*temp = val.intval * 100;

	return ret;
}

static struct thermal_zone_device_ops psy_tzd_ops = {
	.get_temp = power_supply_read_temp,
};

static int psy_register_thermal(struct power_supply *psy)
{
	int i, ret;

	if (psy->desc->no_thermal)
		return 0;

	/* Register battery zone device psy reports temperature */
	for (i = 0; i < psy->desc->num_properties; i++) {
		if (psy->desc->properties[i] == POWER_SUPPLY_PROP_TEMP) {
			psy->tzd = thermal_zone_device_register(psy->desc->name,
					0, 0, psy, &psy_tzd_ops, NULL, 0, 0);
			if (IS_ERR(psy->tzd))
				return PTR_ERR(psy->tzd);
			ret = thermal_zone_device_enable(psy->tzd);
			if (ret)
				thermal_zone_device_unregister(psy->tzd);
			return ret;
		}
	}
	return 0;
}

static void psy_unregister_thermal(struct power_supply *psy)
{
	if (IS_ERR_OR_NULL(psy->tzd))
		return;
	thermal_zone_device_unregister(psy->tzd);
}

/* thermal cooling device callbacks */
static int ps_get_max_charge_cntl_limit(struct thermal_cooling_device *tcd,
					unsigned long *state)
{
	struct power_supply *psy;
	union power_supply_propval val;
	int ret;

	psy = tcd->devdata;
	ret = power_supply_get_property(psy,
			POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX, &val);
	if (ret)
		return ret;

	*state = val.intval;

	return ret;
}

static int ps_get_cur_charge_cntl_limit(struct thermal_cooling_device *tcd,
					unsigned long *state)
{
	struct power_supply *psy;
	union power_supply_propval val;
	int ret;

	psy = tcd->devdata;
	ret = power_supply_get_property(psy,
			POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT, &val);
	if (ret)
		return ret;

	*state = val.intval;

	return ret;
}

static int ps_set_cur_charge_cntl_limit(struct thermal_cooling_device *tcd,
					unsigned long state)
{
	struct power_supply *psy;
	union power_supply_propval val;
	int ret;

	psy = tcd->devdata;
	val.intval = state;
	ret = psy->desc->set_property(psy,
		POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT, &val);

	return ret;
}

static const struct thermal_cooling_device_ops psy_tcd_ops = {
	.get_max_state = ps_get_max_charge_cntl_limit,
	.get_cur_state = ps_get_cur_charge_cntl_limit,
	.set_cur_state = ps_set_cur_charge_cntl_limit,
};

static int psy_register_cooler(struct power_supply *psy)
{
	int i;

	/* Register for cooling device if psy can control charging */
	for (i = 0; i < psy->desc->num_properties; i++) {
		if (psy->desc->properties[i] ==
				POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT) {
			if (psy->dev.parent)
				psy->tcd = thermal_of_cooling_device_register(
						dev_of_node(psy->dev.parent),
						(char *)psy->desc->name,
						psy, &psy_tcd_ops);
			else
				psy->tcd = thermal_cooling_device_register(
						(char *)psy->desc->name,
						psy, &psy_tcd_ops);
			return PTR_ERR_OR_ZERO(psy->tcd);
		}
	}
	return 0;
}

static void psy_unregister_cooler(struct power_supply *psy)
{
	if (IS_ERR_OR_NULL(psy->tcd))
		return;
	thermal_cooling_device_unregister(psy->tcd);
}
#else
static int psy_register_thermal(struct power_supply *psy)
{
	return 0;
}

static void psy_unregister_thermal(struct power_supply *psy)
{
}

static int psy_register_cooler(struct power_supply *psy)
{
	return 0;
}

static void psy_unregister_cooler(struct power_supply *psy)
{
}
#endif

static struct power_supply *__must_check
__power_supply_register(struct device *parent,
				   const struct power_supply_desc *desc,
				   const struct power_supply_config *cfg,
				   bool ws)
{
	struct device *dev;
	struct power_supply *psy;
	int i, rc;

	if (!parent)
		pr_warn("%s: Expected proper parent device for '%s'\n",
			__func__, desc->name);

	if (!desc || !desc->name || !desc->properties || !desc->num_properties)
		return ERR_PTR(-EINVAL);

	for (i = 0; i < desc->num_properties; ++i) {
		if ((desc->properties[i] == POWER_SUPPLY_PROP_USB_TYPE) &&
		    (!desc->usb_types || !desc->num_usb_types))
			return ERR_PTR(-EINVAL);
	}

	psy = kzalloc(sizeof(*psy), GFP_KERNEL);
	if (!psy)
		return ERR_PTR(-ENOMEM);

	dev = &psy->dev;

	device_initialize(dev);

	dev->class = power_supply_class;
	dev->type = &power_supply_dev_type;
	dev->parent = parent;
	dev->release = power_supply_dev_release;
	dev_set_drvdata(dev, psy);
	psy->desc = desc;
	if (cfg) {
		dev->groups = cfg->attr_grp;
		psy->drv_data = cfg->drv_data;
		psy->of_node =
			cfg->fwnode ? to_of_node(cfg->fwnode) : cfg->of_node;
		psy->supplied_to = cfg->supplied_to;
		psy->num_supplicants = cfg->num_supplicants;
	}

	rc = dev_set_name(dev, "%s", desc->name);
	if (rc)
		goto dev_set_name_failed;

	INIT_WORK(&psy->changed_work, power_supply_changed_work);
	INIT_DELAYED_WORK(&psy->deferred_register_work,
			  power_supply_deferred_register_work);

	rc = power_supply_check_supplies(psy);
	if (rc) {
		dev_info(dev, "Not all required supplies found, defer probe\n");
		goto check_supplies_failed;
	}

	spin_lock_init(&psy->changed_lock);
	rc = device_add(dev);
	if (rc)
		goto device_add_failed;

	rc = device_init_wakeup(dev, ws);
	if (rc)
		goto wakeup_init_failed;

	rc = psy_register_thermal(psy);
	if (rc)
		goto register_thermal_failed;

	rc = power_supply_create_triggers(psy);
	if (rc)
		goto create_triggers_failed;

	rc = power_supply_add_hwmon_sysfs(psy);
	if (rc)
		goto add_hwmon_sysfs_failed;

	/*
	 * Update use_cnt after any uevents (most notably from device_add()).
	 * We are here still during driver's probe but
	 * the power_supply_uevent() calls back driver's get_property
	 * method so:
	 * 1. Driver did not assigned the returned struct power_supply,
	 * 2. Driver could not finish initialization (anything in its probe
	 *    after calling power_supply_register()).
	 */
	atomic_inc(&psy->use_cnt);
	psy->initialized = true;

	queue_delayed_work(system_power_efficient_wq,
			   &psy->deferred_register_work,
			   POWER_SUPPLY_DEFERRED_REGISTER_TIME);

	return psy;

add_hwmon_sysfs_failed:
	power_supply_remove_triggers(psy);
create_triggers_failed:
	psy_unregister_thermal(psy);
register_thermal_failed:
wakeup_init_failed:
	device_del(dev);
device_add_failed:
check_supplies_failed:
dev_set_name_failed:
	put_device(dev);
	return ERR_PTR(rc);
}

/**
 * power_supply_register() - Register new power supply
 * @parent:	Device to be a parent of power supply's device, usually
 *		the device which probe function calls this
 * @desc:	Description of power supply, must be valid through whole
 *		lifetime of this power supply
 * @cfg:	Run-time specific configuration accessed during registering,
 *		may be NULL
 *
 * Return: A pointer to newly allocated power_supply on success
 * or ERR_PTR otherwise.
 * Use power_supply_unregister() on returned power_supply pointer to release
 * resources.
 */
struct power_supply *__must_check power_supply_register(struct device *parent,
		const struct power_supply_desc *desc,
		const struct power_supply_config *cfg)
{
	return __power_supply_register(parent, desc, cfg, true);
}
EXPORT_SYMBOL_GPL(power_supply_register);

/**
 * power_supply_register_no_ws() - Register new non-waking-source power supply
 * @parent:	Device to be a parent of power supply's device, usually
 *		the device which probe function calls this
 * @desc:	Description of power supply, must be valid through whole
 *		lifetime of this power supply
 * @cfg:	Run-time specific configuration accessed during registering,
 *		may be NULL
 *
 * Return: A pointer to newly allocated power_supply on success
 * or ERR_PTR otherwise.
 * Use power_supply_unregister() on returned power_supply pointer to release
 * resources.
 */
struct power_supply *__must_check
power_supply_register_no_ws(struct device *parent,
		const struct power_supply_desc *desc,
		const struct power_supply_config *cfg)
{
	return __power_supply_register(parent, desc, cfg, false);
}
EXPORT_SYMBOL_GPL(power_supply_register_no_ws);

static void devm_power_supply_release(struct device *dev, void *res)
{
	struct power_supply **psy = res;

	power_supply_unregister(*psy);
}

/**
 * devm_power_supply_register() - Register managed power supply
 * @parent:	Device to be a parent of power supply's device, usually
 *		the device which probe function calls this
 * @desc:	Description of power supply, must be valid through whole
 *		lifetime of this power supply
 * @cfg:	Run-time specific configuration accessed during registering,
 *		may be NULL
 *
 * Return: A pointer to newly allocated power_supply on success
 * or ERR_PTR otherwise.
 * The returned power_supply pointer will be automatically unregistered
 * on driver detach.
 */
struct power_supply *__must_check
devm_power_supply_register(struct device *parent,
		const struct power_supply_desc *desc,
		const struct power_supply_config *cfg)
{
	struct power_supply **ptr, *psy;

	ptr = devres_alloc(devm_power_supply_release, sizeof(*ptr), GFP_KERNEL);

	if (!ptr)
		return ERR_PTR(-ENOMEM);
	psy = __power_supply_register(parent, desc, cfg, true);
	if (IS_ERR(psy)) {
		devres_free(ptr);
	} else {
		*ptr = psy;
		devres_add(parent, ptr);
	}
	return psy;
}
EXPORT_SYMBOL_GPL(devm_power_supply_register);

/**
 * devm_power_supply_register_no_ws() - Register managed non-waking-source power supply
 * @parent:	Device to be a parent of power supply's device, usually
 *		the device which probe function calls this
 * @desc:	Description of power supply, must be valid through whole
 *		lifetime of this power supply
 * @cfg:	Run-time specific configuration accessed during registering,
 *		may be NULL
 *
 * Return: A pointer to newly allocated power_supply on success
 * or ERR_PTR otherwise.
 * The returned power_supply pointer will be automatically unregistered
 * on driver detach.
 */
struct power_supply *__must_check
devm_power_supply_register_no_ws(struct device *parent,
		const struct power_supply_desc *desc,
		const struct power_supply_config *cfg)
{
	struct power_supply **ptr, *psy;

	ptr = devres_alloc(devm_power_supply_release, sizeof(*ptr), GFP_KERNEL);

	if (!ptr)
		return ERR_PTR(-ENOMEM);
	psy = __power_supply_register(parent, desc, cfg, false);
	if (IS_ERR(psy)) {
		devres_free(ptr);
	} else {
		*ptr = psy;
		devres_add(parent, ptr);
	}
	return psy;
}
EXPORT_SYMBOL_GPL(devm_power_supply_register_no_ws);

/**
 * power_supply_unregister() - Remove this power supply from system
 * @psy:	Pointer to power supply to unregister
 *
 * Remove this power supply from the system. The resources of power supply
 * will be freed here or on last power_supply_put() call.
 */
void power_supply_unregister(struct power_supply *psy)
{
	WARN_ON(atomic_dec_return(&psy->use_cnt));
	psy->removing = true;
	cancel_work_sync(&psy->changed_work);
	cancel_delayed_work_sync(&psy->deferred_register_work);
	sysfs_remove_link(&psy->dev.kobj, "powers");
	power_supply_remove_hwmon_sysfs(psy);
	power_supply_remove_triggers(psy);
	psy_unregister_cooler(psy);
	psy_unregister_thermal(psy);
	device_init_wakeup(&psy->dev, false);
	device_unregister(&psy->dev);
}
EXPORT_SYMBOL_GPL(power_supply_unregister);

void *power_supply_get_drvdata(struct power_supply *psy)
{
	return psy->drv_data;
}
EXPORT_SYMBOL_GPL(power_supply_get_drvdata);

static int __init power_supply_class_init(void)
{
	power_supply_class = class_create(THIS_MODULE, "power_supply");

	if (IS_ERR(power_supply_class))
		return PTR_ERR(power_supply_class);

	power_supply_class->dev_uevent = power_supply_uevent;
	power_supply_init_attrs(&power_supply_dev_type);

	chihiro_init_proc_entry();

	return 0;
}

static void __exit power_supply_class_exit(void)
{
	class_destroy(power_supply_class);
}

subsys_initcall(power_supply_class_init);
module_exit(power_supply_class_exit);

MODULE_DESCRIPTION("Universal power supply monitor class");
MODULE_AUTHOR("Ian Molton <spyro@f2s.com>, "
	      "Szabolcs Gyurko, "
	      "Anton Vorontsov <cbou@mail.ru>");
MODULE_LICENSE("GPL");
