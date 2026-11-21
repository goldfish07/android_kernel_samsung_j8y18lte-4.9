// SPDX-License-Identifier: GPL-2.0
#include <linux/err.h>
#include <linux/sec_power_supply.h>

int sec_power_supply_register(struct device *parent, struct sec_power_supply *supply,
			      void *driver_data)
{
	struct power_supply_config config = {
		.drv_data = driver_data,
		.of_node = parent->of_node,
		.supplied_to = supply->supplied_to,
		.num_supplicants = supply->num_supplicants,
	};

	supply->desc.name = supply->name;
	supply->desc.type = supply->type;
	supply->desc.properties = supply->properties;
	supply->desc.num_properties = supply->num_properties;
	supply->desc.get_property = supply->get_property;
	supply->desc.set_property = supply->set_property;
	supply->desc.property_is_writeable = supply->property_is_writeable;
	supply->desc.external_power_changed = supply->external_power_changed;
	supply->desc.use_for_apm = supply->use_for_apm;
	supply->desc.no_thermal = supply->no_thermal;
	supply->psy = power_supply_register(parent, &supply->desc, &config);
	if (IS_ERR(supply->psy)) {
		int error = PTR_ERR(supply->psy);

		supply->psy = NULL;
		return error;
	}
	supply->dev = &supply->psy->dev;
	return 0;
}

void sec_power_supply_unregister(struct sec_power_supply *supply)
{
	if (supply->psy) {
		power_supply_unregister(supply->psy);
		supply->psy = NULL;
		supply->dev = NULL;
	}
}

void sec_power_supply_changed(struct sec_power_supply *supply)
{
	if (supply->psy)
		power_supply_changed(supply->psy);
}
