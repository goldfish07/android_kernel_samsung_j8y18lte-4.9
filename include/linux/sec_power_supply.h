/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SEC_POWER_SUPPLY_H
#define _LINUX_SEC_POWER_SUPPLY_H

#include <linux/power_supply.h>

/* Samsung 3.18 driver state, backed by a real 4.9 power-supply object. */
struct sec_power_supply {
	const char *name;
	enum power_supply_type type;
	enum power_supply_property *properties;
	size_t num_properties;
	char **supplied_to;
	size_t num_supplicants;
	int (*get_property)(struct power_supply *, enum power_supply_property,
			    union power_supply_propval *);
	int (*set_property)(struct power_supply *, enum power_supply_property,
			    const union power_supply_propval *);
	int (*property_is_writeable)(struct power_supply *, enum power_supply_property);
	void (*external_power_changed)(struct power_supply *);
	int use_for_apm;
	bool no_thermal;
	struct power_supply_desc desc;
	struct power_supply *psy;
	struct device *dev;
};

int sec_power_supply_register(struct device *parent, struct sec_power_supply *supply,
			      void *driver_data);
void sec_power_supply_unregister(struct sec_power_supply *supply);
void sec_power_supply_changed(struct sec_power_supply *supply);

#endif
