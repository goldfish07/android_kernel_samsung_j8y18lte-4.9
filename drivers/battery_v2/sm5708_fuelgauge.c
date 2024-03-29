/* drivers/battery/sm5708_fuelgauge.c
 * SM5708 Voltage Tracking Fuelgauge Driver
 *
 * Copyright (C) 2013
 * Author: Dongik Sin <dongik.sin@samsung.com>
 * Modified by SW Jung
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#include "include/fuelgauge/sm5708_fuelgauge.h"
#include "include/fuelgauge/sm5708_fuelgauge_impl.h"
#include <linux/uaccess.h>
#include <linux/of.h>
#include <linux/fs.h>
#include <linux/math64.h>
#include <linux/compiler.h>
#include <linux/of_gpio.h>

#define SM5708_FG_DEVICE_NAME "sm5708-fg"
#define ALIAS_NAME "sm5708-fuelgauge"

#define FG_DET_BAT_PRESENT 1
#define MINVAL(a, b) ((a <= b) ? a : b)
#define MAXVAL(a, b) ((a > b) ? a : b)

#define LIMIT_N_CURR_MIXFACTOR -2000
#define FG_ABNORMAL_RESET -1
#define IGNORE_N_I_OFFSET 1
#define ABSOLUTE_ERROR_OCV_MATCH 1
#define SM5708_FG_FULL_DEBUG 0

enum battery_table_type {
	DISCHARGE_TABLE = 0,
	Q_TABLE,
	TABLE_MAX,
};

static int sm5708_device_id = -1;

static struct device_attribute sm5708_fg_attrs[] = {
	SM5708_FG_ATTR(reg),
	SM5708_FG_ATTR(data),
	SM5708_FG_ATTR(regs),
};

static enum power_supply_property sm5708_fuelgauge_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_AVG,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_ENERGY_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TEMP_AMBIENT,
	POWER_SUPPLY_PROP_ENERGY_FULL,
	POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
};

bool sm5708_fg_fuelalert_init(struct i2c_client *client, int soc);
static int sm5708_abnormal_reset_check(struct i2c_client *client);

static inline int sm5708_fg_read_device(struct i2c_client *client,
					  int reg, int bytes, void *dest)
{
	int ret;

	if (bytes > 1)
		ret = i2c_smbus_read_i2c_block_data(client, reg, bytes, dest);
	else {
		ret = i2c_smbus_read_byte_data(client, reg);
		if (ret < 0)
			return ret;
		*(unsigned char *)dest = (unsigned char)ret;
	}
	return ret;
}

static int32_t sm5708_fg_i2c_read_word(struct i2c_client *client,
						uint8_t reg_addr)
{
	uint16_t data = 0;
	int ret;
	ret = sm5708_fg_read_device(client, reg_addr, 2, &data);
	/* pr_info("%s: ret = %d, addr = 0x%x, data = 0x%x\n", __func__, ret, reg_addr, data); */

	if (ret < 0)
		return ret;
	else
		return data;

	/* not use big endian */
	/* return (int32_t)be16_to_cpu(data); */
}


static int32_t sm5708_fg_i2c_write_word(struct i2c_client *client,
							uint8_t reg_addr, uint16_t data)
{
	int ret;

	/*	not use big endian */
	/* data = cpu_to_be16(data); */
	ret = i2c_smbus_write_i2c_block_data(client, reg_addr, 2, (uint8_t *)&data);
	/* pr_info("%s: ret = %d, addr = 0x%x, data = 0x%x\n", __func__, ret, reg_addr, data); */

	return ret;
}

static int32_t sm5708_fg_i2c_verified_write_word(struct i2c_client *client,
		uint8_t reg_addr, uint16_t data)
{
	int ret;

	/*	not use big endian */
	/* data = cpu_to_be16(data); */
	ret = i2c_smbus_write_i2c_block_data(client, reg_addr, 2, (uint8_t *)&data);
	if (ret < 0) {
		msleep(50);
		pr_info("1st fail i2c write %s: ret = %d, addr = 0x%x, data = 0x%x\n",
				__func__, ret, reg_addr, data);
		ret = i2c_smbus_write_i2c_block_data(client, reg_addr, 2, (uint8_t *)&data);
		if (ret < 0) {
			msleep(50);
			pr_info("2nd fail i2c write %s: ret = %d, addr = 0x%x, data = 0x%x\n",
					__func__, ret, reg_addr, data);
			ret = i2c_smbus_write_i2c_block_data(client, reg_addr, 2, (uint8_t *)&data);
			if (ret < 0) {
				pr_info("3rd fail i2c write %s: ret = %d, addr = 0x%x, data = 0x%x\n",
						__func__, ret, reg_addr, data);
			}
		}
	}
	/* pr_info("%s: ret = %d, addr = 0x%x, data = 0x%x\n", __func__, ret, reg_addr, data); */

	return ret;
}

static int sm5708_get_all_value(struct i2c_client *client);
static unsigned int sm5708_get_vbat(struct i2c_client *client);
static unsigned int sm5708_get_ocv(struct i2c_client *client);
static int sm5708_get_curr(struct i2c_client *client);
static int sm5708_get_temperature(struct i2c_client *client);
static unsigned int sm5708_get_soc(struct i2c_client *client);

#if 0
static void sm5708_pr_ver_info(struct i2c_client *client)
{
	pr_info("SM5708 Fuel-Gauge Ver %s\n", FG_DRIVER_VER);
}
#endif

static unsigned int sm5708_get_ocv(struct i2c_client *client)
{
	int ret;
	unsigned int ocv;/* = 3500; 3500 means 3500mV */
	struct sec_fuelgauge_info *fuelgauge = i2c_get_clientdata(client);

	ret = sm5708_fg_i2c_read_word(client, SM5708_REG_OCV);
	if (ret < 0) {
		pr_err("%s: read ocv reg fail\n", __func__);
		ocv = 4000;
	} else {
		ocv = ((ret&0x7800)>>11) * 1000; /* integer; */
		ocv = ocv + (((ret&0x07ff)*1000)/2048); /* integer + fractional */
	}

	fuelgauge->info.batt_ocv = ocv;
#ifdef SM5708_FG_FULL_DEBUG
	pr_info("%s: read = 0x%x, ocv = %d\n", __func__, ret, ocv);
#endif

	return ocv;
}

void sm5708_cal_avg_vbat(struct sec_fuelgauge_info *fuelgauge)
{
	if (fuelgauge->info.batt_avgvoltage == 0)
		fuelgauge->info.batt_avgvoltage = fuelgauge->info.batt_voltage;

	else if (fuelgauge->info.batt_voltage == 0 && fuelgauge->info.p_batt_voltage == 0)
		fuelgauge->info.batt_avgvoltage = 3400;

	else if (fuelgauge->info.batt_voltage == 0)
		fuelgauge->info.batt_avgvoltage =
				((fuelgauge->info.batt_avgvoltage) + (fuelgauge->info.p_batt_voltage))/2;

	else if (fuelgauge->info.p_batt_voltage == 0)
		fuelgauge->info.batt_avgvoltage =
				((fuelgauge->info.batt_avgvoltage) + (fuelgauge->info.batt_voltage))/2;

	else
		fuelgauge->info.batt_avgvoltage =
				((fuelgauge->info.batt_avgvoltage*2) +
				 (fuelgauge->info.p_batt_voltage+fuelgauge->info.batt_voltage))/4;

#ifdef SM5708_FG_FULL_DEBUG
	pr_info("%s: batt_avgvoltage = %d\n", __func__, fuelgauge->info.batt_avgvoltage);
#endif

	return;
}

static unsigned int sm5708_get_vbat(struct i2c_client *client)
{
	int ret;
	struct sec_fuelgauge_info *fuelgauge = i2c_get_clientdata(client);

	unsigned int vbat;/* = 3500; 3500 means 3500mV*/

	ret = sm5708_fg_i2c_read_word(client, SM5708_REG_VOLTAGE);
	if (ret < 0) {
		pr_err("%s: read vbat reg fail", __func__);
		vbat = 4000;
	} else {
		vbat = ((ret&0x3800)>>11) * 1000; /* integer; */
		vbat = vbat + (((ret&0x07ff)*1000)/2048); /* integer + fractional */
	}
	fuelgauge->info.batt_voltage = vbat;

#ifdef SM5708_FG_FULL_DEBUG
	pr_info("%s: read = 0x%x, vbat = %d\n", __func__, ret, vbat);
#endif
	sm5708_cal_avg_vbat(fuelgauge);

	if ((fuelgauge->info.volt_alert_flag == true) && vbat > 3400) {
		fuelgauge->info.volt_alert_flag = false;
		if (fuelgauge->is_fuel_alerted)
			wake_unlock(&fuelgauge->fuel_alert_wake_lock);
		sm5708_fg_fuelalert_init(client,
				fuelgauge->pdata->fuel_alert_soc);
		pr_info("%s : volt_alert_flag = %d\n", __func__, fuelgauge->info.volt_alert_flag);
	}

	return vbat;
}

void sm5708_cal_avg_current(struct sec_fuelgauge_info *fuelgauge)
{
	if (fuelgauge->info.batt_avgcurrent == 0)
		fuelgauge->info.batt_avgcurrent = fuelgauge->info.batt_current;

	else if (fuelgauge->info.batt_avgcurrent == 0 && fuelgauge->info.p_batt_current == 0)
		fuelgauge->info.batt_avgcurrent = fuelgauge->info.batt_current;

	else if (fuelgauge->info.batt_current == 0)
		fuelgauge->info.batt_avgcurrent =
				((fuelgauge->info.batt_avgcurrent) + (fuelgauge->info.p_batt_current))/2;

	else if (fuelgauge->info.p_batt_current == 0)
		fuelgauge->info.batt_avgcurrent =
				((fuelgauge->info.batt_avgcurrent) + (fuelgauge->info.batt_current))/2;

	else
		fuelgauge->info.batt_avgcurrent =
				((fuelgauge->info.batt_avgcurrent*2) +
				 (fuelgauge->info.p_batt_current+fuelgauge->info.batt_current))/4;

#ifdef SM5708_FG_FULL_DEBUG
	pr_info("%s: batt_avgcurrent = %d\n", __func__, fuelgauge->info.batt_avgcurrent);
#endif

	return;
}


static int sm5708_get_curr(struct i2c_client *client)
{
	int ret, volt_slope, mohm_volt_cal;
	struct sec_fuelgauge_info *fuelgauge = i2c_get_clientdata(client);

	int curr;/* = 1000; 1000 means 1000mA*/

	ret = sm5708_fg_i2c_read_word(client, SM5708_REG_CURRENT);
	if (ret < 0) {
		pr_err("%s: read curr reg fail", __func__);
		curr = 0;
	} else {
		curr = ((ret&0x1800)>>11) * 1000; /* integer; */
		curr = curr + (((ret&0x07ff)*1000)/2048); /* integer + fractional */

		if (ret&0x8000) {
			curr *= -1;
		}
	}
	fuelgauge->info.batt_current = curr;

#ifdef SM5708_FG_FULL_DEBUG
		pr_info("%s: read = 0x%x, curr = %d\n", __func__, ret, curr);
#endif

	/* set vbat offset cancel start */
	volt_slope = sm5708_fg_i2c_read_word(client, SM5708_REG_VOLT_CAL);
	volt_slope = volt_slope & 0xFF00;
	mohm_volt_cal = fuelgauge->info.volt_cal & 0x00FF;
	if (fuelgauge->info.enable_v_offset_cancel_p) {
		if (fuelgauge->is_charging && (curr > fuelgauge->info.v_offset_cancel_level)) {
			if (mohm_volt_cal & 0x0080) {
				mohm_volt_cal = -(mohm_volt_cal & 0x007F);
			}
			mohm_volt_cal = mohm_volt_cal - (curr/(fuelgauge->info.v_offset_cancel_mohm * 13)); /* ((curr*0.001)*0.006)*2048 -> 6mohm */
			if (mohm_volt_cal < 0) {
				mohm_volt_cal = -mohm_volt_cal;
				mohm_volt_cal = mohm_volt_cal|0x0080;
			}
		}
	}
	if (fuelgauge->info.enable_v_offset_cancel_n) {
		if (!(fuelgauge->is_charging) && (curr < -(fuelgauge->info.v_offset_cancel_level))) {
			if (fuelgauge->info.volt_cal & 0x0080) {
				mohm_volt_cal = -(mohm_volt_cal & 0x007F);
			}
			mohm_volt_cal = mohm_volt_cal - (curr/(fuelgauge->info.v_offset_cancel_mohm * 13)); /* ((curr*0.001)*0.006)*2048 -> 6mohm */
			if (mohm_volt_cal < 0) {
				mohm_volt_cal = -mohm_volt_cal;
				mohm_volt_cal = mohm_volt_cal|0x0080;
			}
		}
	}
	sm5708_fg_i2c_write_word(client, SM5708_REG_VOLT_CAL, (mohm_volt_cal | volt_slope));
	pr_info("%s: <%d %d %d %d> info.volt_cal = 0x%x, volt_slope = 0x%x, mohm_volt_cal = 0x%x\n",
			__func__, fuelgauge->info.enable_v_offset_cancel_p, fuelgauge->info.enable_v_offset_cancel_n
			, fuelgauge->info.v_offset_cancel_level, fuelgauge->info.v_offset_cancel_mohm
			, fuelgauge->info.volt_cal, volt_slope, mohm_volt_cal);
	/* set vbat offset cancel end */

	sm5708_cal_avg_current(fuelgauge);

	return curr;
}

static int sm5708_get_temperature(struct i2c_client *client)
{
	int ret;
	struct sec_fuelgauge_info *fuelgauge = i2c_get_clientdata(client);

	int temp;/* = 250; 250 means 25.0oC*/
	/* double temp_data; */

	ret = sm5708_fg_i2c_read_word(client, SM5708_REG_TEMPERATURE);
	if (ret < 0) {
		pr_err("%s: read temp reg fail", __func__);
		temp = 0;
	} else {
		temp = ((ret&0x7F00)>>8) * 10; /* integer bit */
		temp = temp + (((ret&0x00f0)*10)/256); /* integer + fractional bit */
		if (ret&0x8000) {
			temp *= -1;
		}
	}
	fuelgauge->info.temp_fg = temp;

#ifdef SM5708_FG_FULL_DEBUG
	pr_info("%s: read = 0x%x, temp_fg = %d\n", __func__, ret, temp);
#endif

	return temp;
}

static int sm5708_get_soc_cycle(struct i2c_client *client)
{
	int ret;
	struct sec_fuelgauge_info *fuelgauge = i2c_get_clientdata(client);

	int cycle;

	ret = sm5708_fg_i2c_read_word(client, SM5708_REG_SOC_CYCLE);
	if (ret < 0) {
		pr_err("%s: read cycle reg fail", __func__);
		cycle = 0;
	} else {
		cycle = ret&0x03FF;
	}
	fuelgauge->info.batt_soc_cycle = cycle;

#ifdef SM5708_FG_FULL_DEBUG
	pr_info("%s: read = 0x%x, soc_cycle = %d\n", __func__, ret, cycle);
#endif

	return cycle;
}

static void sm5708_fg_test_read(struct i2c_client *client)
{
	int ret0, ret1, ret2, ret3, ret4, ret5, ret6, ret7, ret8, ret9, ret10, ret11;

	ret0 = sm5708_fg_i2c_read_word(client, 0xA0);
	ret1 = sm5708_fg_i2c_read_word(client, 0xAC);
	ret2 = sm5708_fg_i2c_read_word(client, 0xAD);
	ret3 = sm5708_fg_i2c_read_word(client, 0xAE);
	ret4 = sm5708_fg_i2c_read_word(client, 0xAF);
	ret5 = sm5708_fg_i2c_read_word(client, 0x28);
	ret6 = sm5708_fg_i2c_read_word(client, 0x2F);
	ret7 = sm5708_fg_i2c_read_word(client, 0x01);
	pr_info("%s: 0xA0=0x%04x, 0xAC=0x%04x, 0xAD=0x%04x, 0xAE=0x%04x, 0xAF=0x%04x, 0x28=0x%04x, 0x2F=0x%04x, 0x01=0x%04x, SM5708_ID=0x%04x\n",
		__func__, ret0, ret1, ret2, ret3, ret4, ret5, ret6, ret7, sm5708_device_id);

	ret0 = sm5708_fg_i2c_read_word(client, 0xB0);
	ret1 = sm5708_fg_i2c_read_word(client, 0xBC);
	ret2 = sm5708_fg_i2c_read_word(client, 0xBD);
	ret3 = sm5708_fg_i2c_read_word(client, 0xBE);
	ret4 = sm5708_fg_i2c_read_word(client, 0xBF);
	ret5 = sm5708_fg_i2c_read_word(client, 0x85);
	ret6 = sm5708_fg_i2c_read_word(client, 0x86);
	ret7 = sm5708_fg_i2c_read_word(client, 0x87);
	ret8 = sm5708_fg_i2c_read_word(client, 0x1F);
	ret9 = sm5708_fg_i2c_read_word(client, 0x94);
	ret10 = sm5708_fg_i2c_read_word(client, 0x13);
	ret11 = sm5708_fg_i2c_read_word(client, 0x14);
	pr_info("%s: 0xB0=0x%04x, 0xBC=0x%04x, 0xBD=0x%04x, 0xBE=0x%04x, 0xBF=0x%04x, 0x85=0x%04x, 0x86=0x%04x, 0x87=0x%04x, 0x1F=0x%04x, 0x94=0x%04x , 0x13=0x%04x, 0x14=0x%04x\n",
		__func__, ret0, ret1, ret2, ret3, ret4, ret5, ret6, ret7, ret8, ret9, ret10, ret11);

	return;
}

static unsigned int sm5708_get_device_id(struct i2c_client *client)
{
	int ret;
	ret = sm5708_fg_i2c_read_word(client, SM5708_REG_DEVICE_ID);
	sm5708_device_id = ret;
	pr_info("%s: SM5708 device_id = 0x%x\n", __func__, ret);

	return ret;
}

int sm5708_call_fg_device_id(void)
{
	pr_info("%s: extern call SM5708 fg_device_id = 0x%x\n", __func__, sm5708_device_id);

	return sm5708_device_id;
}

static bool sm5708_fg_check_reg_init_need(struct i2c_client *client)
{
	int ret;

	ret = sm5708_fg_i2c_read_word(client, SM5708_REG_FG_OP_STATUS);

	if ((ret & INIT_CHECK_MASK) == DISABLE_RE_INIT) {
		pr_info("%s: SM5708_REG_FG_OP_STATUS : 0x%x , return 0\n", __func__, ret);
		return 0;
	} else {
		pr_info("%s: SM5708_REG_FG_OP_STATUS : 0x%x , return 1\n", __func__, ret);
		return 1;
	}
}

unsigned int sm5708_get_soc(struct i2c_client *client)
{
	int ret;
	unsigned int soc;

	struct sec_fuelgauge_info *fuelgauge = i2c_get_clientdata(client);

	ret = sm5708_fg_i2c_read_word(client, SM5708_REG_SOC);
	if (ret < 0) {
		pr_err("%s: Warning!!!! read soc reg fail\n", __func__);
		soc = 500;
	} else {
		soc = ((ret&0xff00)>>8) * 10; /* integer bit; */
		soc = soc + (((ret&0x00ff)*10)/256); /* integer + fractional bit */
	}

	if (sm5708_abnormal_reset_check(client) < 0) {
		pr_info("%s: FG init ERROR!! pre_SOC returned!!, read_SOC = %d, pre_SOC = %d\n", __func__, soc, fuelgauge->info.batt_soc);
		return fuelgauge->info.batt_soc;
	}

#ifdef SM5708_FG_FULL_DEBUG
	pr_info("%s: read = 0x%x, soc = %d\n", __func__, ret, soc);
#endif

	/* for low temp power off test */
	if (fuelgauge->info.volt_alert_flag && (fuelgauge->info.temperature < -100)) {
		pr_info("%s: volt_alert_flag is TRUE!!!! SOC make force ZERO!!!!\n", __func__);
		fuelgauge->info.batt_soc = 0;
		return 0;
	}
	fuelgauge->info.batt_soc = soc;

	return soc;
}

static void sm5708_fg_buffer_read(struct i2c_client *client)
{
	int ret0, ret1, ret2, ret3, ret4, ret5;

	ret0 = sm5708_fg_i2c_read_word(client, 0x30);
	ret1 = sm5708_fg_i2c_read_word(client, 0x31);
	ret2 = sm5708_fg_i2c_read_word(client, 0x32);
	ret3 = sm5708_fg_i2c_read_word(client, 0x33);
	ret4 = sm5708_fg_i2c_read_word(client, 0x34);
	ret5 = sm5708_fg_i2c_read_word(client, 0x35);
	pr_info("%s: sm5708 FG buffer 0x30_0x35 lb_V = 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x\n",
		__func__, ret0, ret1, ret2, ret3, ret4, ret5);

	ret0 = sm5708_fg_i2c_read_word(client, 0x36);
	ret1 = sm5708_fg_i2c_read_word(client, 0x37);
	ret2 = sm5708_fg_i2c_read_word(client, 0x38);
	ret3 = sm5708_fg_i2c_read_word(client, 0x39);
	ret4 = sm5708_fg_i2c_read_word(client, 0x3A);
	ret5 = sm5708_fg_i2c_read_word(client, 0x3B);
	pr_info("%s: sm5708 FG buffer 0x36_0x3B cb_V = 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x\n",
		__func__, ret0, ret1, ret2, ret3, ret4, ret5);


	ret0 = sm5708_fg_i2c_read_word(client, 0x40);
	ret1 = sm5708_fg_i2c_read_word(client, 0x41);
	ret2 = sm5708_fg_i2c_read_word(client, 0x42);
	ret3 = sm5708_fg_i2c_read_word(client, 0x43);
	ret4 = sm5708_fg_i2c_read_word(client, 0x44);
	ret5 = sm5708_fg_i2c_read_word(client, 0x45);
	pr_info("%s: sm5708 FG buffer 0x40_0x45 lb_I = 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x\n",
		__func__, ret0, ret1, ret2, ret3, ret4, ret5);


	ret0 = sm5708_fg_i2c_read_word(client, 0x46);
	ret1 = sm5708_fg_i2c_read_word(client, 0x47);
	ret2 = sm5708_fg_i2c_read_word(client, 0x48);
	ret3 = sm5708_fg_i2c_read_word(client, 0x49);
	ret4 = sm5708_fg_i2c_read_word(client, 0x4A);
	ret5 = sm5708_fg_i2c_read_word(client, 0x4B);
	pr_info("%s: sm5708 FG buffer 0x46_0x4B cb_I = 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x\n",
		__func__, ret0, ret1, ret2, ret3, ret4, ret5);

	return;
}

static bool sm5708_fg_get_batt_present(struct i2c_client *client)
{
	/* SM5708 is not suport batt present */
	dev_dbg(&client->dev, "%s: sm5708_fg_get_batt_present\n", __func__);

	return true;
}

int sm5708_calculate_iocv(struct i2c_client *client)
{
	bool only_lb = false, sign_i_offset = 0;
	int roop_start = 0, roop_max = 0, i = 0, cb_last_index = 0, cb_pre_last_index = 0;
	int lb_v_buffer[6] = {0, 0, 0, 0, 0, 0};
	int lb_i_buffer[6] = {0, 0, 0, 0, 0, 0};
	int cb_v_buffer[6] = {0, 0, 0, 0, 0, 0};
	int cb_i_buffer[6] = {0, 0, 0, 0, 0, 0};
	int i_offset_margin = 0x14, i_vset_margin = 0x67;
	int v_max = 0, v_min = 0, v_sum = 0, lb_v_avg = 0, cb_v_avg = 0, lb_v_set = 0, lb_i_set = 0, i_offset = 0;
	int i_max = 0, i_min = 0, i_sum = 0, lb_i_avg = 0, cb_i_avg = 0, cb_v_set = 0, cb_i_set = 0;
	int lb_i_p_v_min = 0, lb_i_n_v_max = 0, cb_i_p_v_min = 0, cb_i_n_v_max = 0;

	int v_ret = 0, i_ret = 0, ret = 0;

	ret = sm5708_fg_i2c_read_word(client, SM5708_REG_END_V_IDX);
	pr_info("%s: iocv_status_read = addr : 0x%x , data : 0x%x\n", __func__, SM5708_REG_END_V_IDX, ret);

	/* init start */
	if ((ret & 0x0010) == 0x0000) {
		only_lb = true;
	}

	/* init end */

	/* lb get start */
	roop_max = (ret & 0x000F);
	if (roop_max > 6)
		roop_max = 6;

	roop_start = SM5708_REG_IOCV_B_L_MIN;
	for (i = roop_start; i < roop_start + roop_max; i++) {
		v_ret = sm5708_fg_i2c_read_word(client, i);
		i_ret = sm5708_fg_i2c_read_word(client, i+0x10);

		if ((i_ret&0x4000) == 0x4000) {
			i_ret = -(i_ret&0x3FFF);
		}

		lb_v_buffer[i-roop_start] = v_ret;
		lb_i_buffer[i-roop_start] = i_ret;

		if (i == roop_start) {
			v_max = v_ret;
			v_min = v_ret;
			v_sum = v_ret;
			i_max = i_ret;
			i_min = i_ret;
			i_sum = i_ret;
		} else {
			if (v_ret > v_max)
				v_max = v_ret;
			else if (v_ret < v_min)
				v_min = v_ret;
			v_sum = v_sum + v_ret;

			if (i_ret > i_max)
				i_max = i_ret;
			else if (i_ret < i_min)
				i_min = i_ret;
			i_sum = i_sum + i_ret;
		}

		if (abs(i_ret) > i_vset_margin) {
			if (i_ret > 0) {
				if (lb_i_p_v_min == 0) {
					lb_i_p_v_min = v_ret;
				} else {
					if (v_ret < lb_i_p_v_min)
						lb_i_p_v_min = v_ret;
				}
			} else {
				if (lb_i_n_v_max == 0) {
					lb_i_n_v_max = v_ret;
				} else {
					if (v_ret > lb_i_n_v_max)
						lb_i_n_v_max = v_ret;
				}
			}
		}
	}
	v_sum = v_sum - v_max - v_min;
	i_sum = i_sum - i_max - i_min;

	lb_v_avg = v_sum / (roop_max-2);
	lb_i_avg = i_sum / (roop_max-2);
	/* lb get end */

	/* lb_vset start */
	if (abs(lb_i_buffer[roop_max-1]) < i_vset_margin) {
		if (abs(lb_i_buffer[roop_max-2]) < i_vset_margin) {
			lb_v_set = MAXVAL(lb_v_buffer[roop_max-2], lb_v_buffer[roop_max-1]);
			if (abs(lb_i_buffer[roop_max-3]) < i_vset_margin) {
				lb_v_set = MAXVAL(lb_v_buffer[roop_max-3], lb_v_set);
			}
		} else {
			lb_v_set = lb_v_buffer[roop_max-1];
		}
	} else {
		lb_v_set = lb_v_avg;
	}

	if (lb_i_n_v_max > 0) {
		lb_v_set = MAXVAL(lb_i_n_v_max, lb_v_set);
	}
	/* else if (lb_i_p_v_min > 0) {
		lb_v_set = MINVAL(lb_i_p_v_min, lb_v_set);
	}
	 lb_vset end */

	/* lb offset make start */
	if (roop_max > 3) {
		lb_i_set = (lb_i_buffer[2] + lb_i_buffer[3]) / 2;
	}

	if ((abs(lb_i_buffer[roop_max-1]) < i_offset_margin) && (abs(lb_i_set) < i_offset_margin)) {
		lb_i_set = MAXVAL(lb_i_buffer[roop_max-1], lb_i_set);
	} else if (abs(lb_i_buffer[roop_max-1]) < i_offset_margin) {
		lb_i_set = lb_i_buffer[roop_max-1];
	} else if (abs(lb_i_set) < i_offset_margin) {
		lb_i_set = lb_i_set;
	} else {
		lb_i_set = 0;
	}

	i_offset = lb_i_set;

	i_offset = i_offset + 4;	/* add extra offset */

	if (i_offset <= 0) {
		sign_i_offset = 1;
#ifdef IGNORE_N_I_OFFSET
		i_offset = 0;
#else
		i_offset = -i_offset;
#endif
	}

	i_offset = i_offset >> 1;

	if (sign_i_offset == 0) {
		i_offset = i_offset|0x0080;
	}

	/* do not write in kernel point. */
	/* sm5708_fg_i2c_write_word(client, SM5708_REG_CURR_OFF, i_offset); */
	/* lb offset make end */

	pr_info("%s: iocv_l_max=0x%x, iocv_l_min=0x%x, iocv_l_avg=0x%x, lb_v_set=0x%x, roop_max=%d\n",
			__func__, v_max, v_min, lb_v_avg, lb_v_set, roop_max);
	pr_info("%s: ioci_l_max=0x%x, ioci_l_min=0x%x, ioci_l_avg=0x%x, lb_i_set=0x%x, i_offset=0x%x, sign_i_offset=%d\n",
			__func__, i_max, i_min, lb_i_avg, lb_i_set, i_offset, sign_i_offset);

	if (!only_lb) {
		/* cb get start */
		roop_start = SM5708_REG_IOCV_B_C_MIN;
		roop_max = 6;
		for (i = roop_start; i < roop_start + roop_max; i++) {
			v_ret = sm5708_fg_i2c_read_word(client, i);
			i_ret = sm5708_fg_i2c_read_word(client, i+0x10);
			if ((i_ret&0x4000) == 0x4000) {
				i_ret = -(i_ret&0x3FFF);
			}

			cb_v_buffer[i-roop_start] = v_ret;
			cb_i_buffer[i-roop_start] = i_ret;

			if (i == roop_start) {
				v_max = v_ret;
				v_min = v_ret;
				v_sum = v_ret;
				i_max = i_ret;
				i_min = i_ret;
				i_sum = i_ret;
			} else {
				if (v_ret > v_max)
					v_max = v_ret;
				else if (v_ret < v_min)
					v_min = v_ret;
				v_sum = v_sum + v_ret;

				if (i_ret > i_max)
					i_max = i_ret;
				else if (i_ret < i_min)
					i_min = i_ret;
				i_sum = i_sum + i_ret;
			}

			if (abs(i_ret) > i_vset_margin) {
				if (i_ret > 0) {
					if (cb_i_p_v_min == 0) {
						cb_i_p_v_min = v_ret;
					} else {
						if (v_ret < cb_i_p_v_min)
							cb_i_p_v_min = v_ret;
					}
				} else {
					if (cb_i_n_v_max == 0) {
						cb_i_n_v_max = v_ret;
					} else {
						if (v_ret > cb_i_n_v_max)
							cb_i_n_v_max = v_ret;
					}
				}
			}
		}
		v_sum = v_sum - v_max - v_min;
		i_sum = i_sum - i_max - i_min;

		cb_v_avg = v_sum / (roop_max - 2);
		cb_i_avg = i_sum / (roop_max - 2);
		/* cb get end */

		/* cb_vset start */
		cb_last_index = (ret & 0x000F) - 7; /* -6-1 */
		if (cb_last_index < 0) {
			cb_last_index = 5;
		}

		for (i = roop_max; i > 0; i--) {
			if (abs(cb_i_buffer[cb_last_index]) < i_vset_margin) {
				cb_v_set = cb_v_buffer[cb_last_index];
				if (abs(cb_i_buffer[cb_last_index]) < i_offset_margin) {
					cb_i_set = cb_i_buffer[cb_last_index];
				}

				cb_pre_last_index = cb_last_index - 1;
				if (cb_pre_last_index < 0) {
					cb_pre_last_index = 5;
				}

				if (abs(cb_i_buffer[cb_pre_last_index]) < i_vset_margin) {
					cb_v_set = MAXVAL(cb_v_buffer[cb_pre_last_index], cb_v_set);
					if (abs(cb_i_buffer[cb_pre_last_index]) < i_offset_margin) {
						cb_i_set = MAXVAL(cb_i_buffer[cb_pre_last_index], cb_i_set);
					}
				}
			} else {
				cb_last_index--;
				if (cb_last_index < 0) {
					cb_last_index = 5;
				}
			}
		}

		if (cb_v_set == 0) {
			cb_v_set = cb_v_avg;
			if (cb_i_set == 0) {
				cb_i_set = cb_i_avg;
			}
		}

		if (cb_i_n_v_max > 0) {
			cb_v_set = MAXVAL(cb_i_n_v_max, cb_v_set);
		}
		/* else if (cb_i_p_v_min > 0) {
			cb_v_set = MINVAL(cb_i_p_v_min, cb_v_set);
		}
		cb_vset end */

		/* cb offset make start */
		if (abs(cb_i_set) < i_offset_margin) {
			if (cb_i_set > lb_i_set) {
				i_offset = cb_i_set;
				i_offset = i_offset + 4;	/* add extra offset */

				if (i_offset <= 0) {
					sign_i_offset = 1;
#ifdef IGNORE_N_I_OFFSET
					i_offset = 0;
#else
					i_offset = -i_offset;
#endif
				}

				i_offset = i_offset >> 1;

				if (sign_i_offset == 0) {
					i_offset = i_offset|0x0080;
				}

				/* do not write in kernel point. */
				/* sm5708_fg_i2c_write_word(client, SM5708_REG_CURR_OFF, i_offset); */
			}
		}
		/* cb offset make end */

		pr_info("%s: iocv_c_max=0x%x, iocv_c_min=0x%x, iocv_c_avg=0x%x, cb_v_set=0x%x, cb_last_index=%d\n",
				__func__, v_max, v_min, cb_v_avg, cb_v_set, cb_last_index);
		pr_info("%s: ioci_c_max=0x%x, ioci_c_min=0x%x, ioci_c_avg=0x%x, cb_i_set=0x%x, i_offset=0x%x, sign_i_offset=%d\n",
				__func__, i_max, i_min, cb_i_avg, cb_i_set, i_offset, sign_i_offset);

	}

	/* final set */
	if ((abs(cb_i_set) > i_vset_margin) || only_lb) {
		ret = MAXVAL(lb_v_set, cb_i_n_v_max);
	} else {
		ret = cb_v_set;
	}

	return ret;
}

static void sm5708_set_soc_cycle_cfg(struct i2c_client *client)
{
	int value;

	struct sec_fuelgauge_info *fuelgauge = i2c_get_clientdata(client);

	value = fuelgauge->info.cycle_limit_cntl|(fuelgauge->info.cycle_high_limit<<12)|(fuelgauge->info.cycle_low_limit<<8);

	pr_info("%s: cycle cfg value = 0x%x\n", __func__, value);

	sm5708_fg_i2c_write_word(client, SM5708_REG_SOC_CYCLE_CFG, value);
}

#if defined(CONFIG_BATTERY_AGE_FORECAST)
int get_v_max_index_by_cycle(struct i2c_client *client)
{
	int cycle_index = 0, len;
	struct sec_fuelgauge_info *fuelgauge = i2c_get_clientdata(client);

	for (len = fuelgauge->pdata->num_age_step-1; len >= 0; --len) {
		if (fuelgauge->chg_full_soc == fuelgauge->pdata->age_data[len].full_condition_soc) {
			cycle_index = len;
		break;
		}
	}
    pr_info("%s: chg_full_soc = %d, index = %d \n", __func__, fuelgauge->chg_full_soc, cycle_index);

    return cycle_index;
}
#endif

static bool sm5708_fg_reg_init(struct i2c_client *client, int is_surge)
{
	int i, j, value, ret;
	uint8_t table_reg;
	int write_table[2][16];
	struct sec_fuelgauge_info *fuelgauge = i2c_get_clientdata(client);

	pr_info("%s: sm5708_fg_reg_init START!!\n", __func__);

	/* init mark */
	sm5708_fg_i2c_write_word(client, SM5708_REG_RESET, SM5708_FG_INIT_MARK);

	/* start first param_ctrl unlock & TABLE_LEN write */
	sm5708_fg_i2c_write_word(client, SM5708_REG_PARAM_CTRL, SM5708_FG_PARAM_UNLOCK_CODE | SM5708_FG_TABLE_LEN);

	/* RCE write */
	for (i = 0; i < 3; i++) {
		sm5708_fg_i2c_write_word(client, SM5708_REG_RCE0+i, fuelgauge->info.rce_value[i]);
		pr_info("%s: RCE write RCE%d = 0x%x : 0x%x\n",
				__func__,  i, SM5708_REG_RCE0+i, fuelgauge->info.rce_value[i]);
	}

	/* DTCD write */
	sm5708_fg_i2c_write_word(client, SM5708_REG_DTCD, fuelgauge->info.dtcd_value);
	pr_info("%s: DTCD write DTCD = 0x%x : 0x%x\n",
			__func__, SM5708_REG_DTCD, fuelgauge->info.dtcd_value);

	/* RS write */
	sm5708_fg_i2c_write_word(client, SM5708_REG_AUTO_RS_MAN, fuelgauge->info.rs_value[0]);
	pr_info("%s: RS write RS = 0x%x : 0x%x\n",
			__func__, SM5708_REG_AUTO_RS_MAN, fuelgauge->info.rs_value[0]);

	/* VIT_PERIOD write */
	sm5708_fg_i2c_write_word(client, SM5708_REG_VIT_PERIOD, fuelgauge->info.vit_period);
	pr_info("%s: VIT_PERIOD write VIT_PERIOD = 0x%x : 0x%x\n",
			__func__, SM5708_REG_VIT_PERIOD, fuelgauge->info.vit_period);

	/* TABLE_LEN write & pram unlock */
	sm5708_fg_i2c_write_word(client, SM5708_REG_PARAM_CTRL,
					SM5708_FG_PARAM_UNLOCK_CODE | SM5708_FG_TABLE_LEN);

#if defined(CONFIG_BATTERY_AGE_FORECAST)
	i = get_v_max_index_by_cycle(client);
	pr_info("%s: v_max_now is change %x -> %x \n", __func__, fuelgauge->info.v_max_now, fuelgauge->info.v_max_table[i]);
	pr_info("%s: q_max_now is change %x -> %x \n", __func__, fuelgauge->info.q_max_now, fuelgauge->info.q_max_table[i]);
	fuelgauge->info.v_max_now = fuelgauge->info.v_max_table[i];
	fuelgauge->info.q_max_now = fuelgauge->info.q_max_table[i];
#endif
	for (i = TABLE_MAX-1; i >= 0; i--) {
		for (j = 0; j <= SM5708_FG_TABLE_LEN; j++) {
#if defined(CONFIG_BATTERY_AGE_FORECAST)
			if (i == Q_TABLE) {
				write_table[i][j] = fuelgauge->info.battery_table[i][j];
				if (j == SM5708_FG_TABLE_LEN) {
					write_table[i][SM5708_FG_TABLE_LEN-1] = fuelgauge->info.q_max_now;
					write_table[i][SM5708_FG_TABLE_LEN] = fuelgauge->info.q_max_now + (fuelgauge->info.q_max_now/1000);
				}
			} else {
				write_table[i][j] = fuelgauge->info.battery_table[i][j];
				if (j == SM5708_FG_TABLE_LEN-1) {
					write_table[i][SM5708_FG_TABLE_LEN-1] = fuelgauge->info.v_max_now;
					if (write_table[i][SM5708_FG_TABLE_LEN-1] < write_table[i][SM5708_FG_TABLE_LEN-2]) {
						write_table[i][SM5708_FG_TABLE_LEN-2] = write_table[i][SM5708_FG_TABLE_LEN-1] - 0x18; // ~11.7mV
						write_table[Q_TABLE][SM5708_FG_TABLE_LEN-2] = (write_table[Q_TABLE][SM5708_FG_TABLE_LEN-1]*99)/100;
					}
				}
			}
#else
			write_table[i][j] = fuelgauge->info.battery_table[i][j];
#endif
		}
	}

	for (i = 0; i < TABLE_MAX; i++) {
		table_reg = SM5708_REG_TABLE_START + (i<<4);
		for (j = 0; j <= SM5708_FG_TABLE_LEN; j++) {
			sm5708_fg_i2c_write_word(client, (table_reg + j), write_table[i][j]);
			msleep(10);
			if (write_table[i][j] != sm5708_fg_i2c_read_word(client, (table_reg + j))) {
				pr_info("%s: TABLE write FAIL retry[%d][%d] = 0x%x : 0x%x\n",
					__func__, i, j, (table_reg + j), write_table[i][j]);
				sm5708_fg_i2c_write_word(client, (table_reg + j), write_table[i][j]);
			}
			pr_info("%s: TABLE write OK [%d][%d] = 0x%x : 0x%x\n",
				__func__, i, j, (table_reg + j), write_table[i][j]);
		}
	}

	/* MIX_MODE write */
	sm5708_fg_i2c_write_word(client, SM5708_REG_RS_MIX_FACTOR, fuelgauge->info.rs_value[2]);
	sm5708_fg_i2c_write_word(client, SM5708_REG_RS_MAX, fuelgauge->info.rs_value[3]);
	sm5708_fg_i2c_write_word(client, SM5708_REG_RS_MIN, fuelgauge->info.rs_value[4]);
	sm5708_fg_i2c_write_word(client, SM5708_REG_MIX_RATE, fuelgauge->info.mix_value[0]);
	sm5708_fg_i2c_write_word(client, SM5708_REG_MIX_INIT_BLANK, fuelgauge->info.mix_value[1]);

	pr_info("%s: RS_MIX_FACTOR = 0x%x, RS_MAX = 0x%x, RS_MIN = 0x%x, MIX_RATE = 0x%x, MIX_INIT_BLANK = 0x%x\n",
		__func__,
		fuelgauge->info.rs_value[2], fuelgauge->info.rs_value[3], fuelgauge->info.rs_value[4],
		fuelgauge->info.mix_value[0], fuelgauge->info.mix_value[1]);

	/* CAL write */
	sm5708_fg_i2c_write_word(client, SM5708_REG_VOLT_CAL, fuelgauge->info.volt_cal);
	sm5708_fg_i2c_write_word(client, SM5708_REG_CURR_OFF, fuelgauge->info.curr_offset);
	sm5708_fg_i2c_write_word(client, SM5708_REG_CURR_P_SLOPE, fuelgauge->info.p_curr_cal);
	sm5708_fg_i2c_write_word(client, SM5708_REG_CURR_N_SLOPE, fuelgauge->info.n_curr_cal);
	pr_info("%s: VOLT_CAL = 0x%x, curr_offset = 0x%x, p_curr_cal = 0x%x, n_curr_cal = 0x%x\n",
		__func__, fuelgauge->info.volt_cal, fuelgauge->info.curr_offset,
		fuelgauge->info.p_curr_cal, fuelgauge->info.n_curr_cal);

	/* MISC write */
	sm5708_fg_i2c_write_word(client, SM5708_REG_MISC, fuelgauge->info.misc);
	pr_info("%s: SM5708_REG_MISC 0x%x : 0x%x\n",
		__func__, SM5708_REG_MISC, fuelgauge->info.misc);

	/* TOPOFF SOC */
	sm5708_fg_i2c_write_word(client, SM5708_REG_TOPOFFSOC, fuelgauge->info.topoff_soc);
	pr_info("%s: SM5708_REG_TOPOFFSOC 0x%x : 0x%x\n", __func__,
		SM5708_REG_TOPOFFSOC, fuelgauge->info.topoff_soc);

	/* INIT_last -	control register set */
	value = sm5708_fg_i2c_read_word(client, SM5708_REG_CNTL);
	if (value == CNTL_REG_DEFAULT_VALUE) {
		value = fuelgauge->info.cntl_value;
	}
	value = ENABLE_MIX_MODE | ENABLE_TEMP_MEASURE | ENABLE_MANUAL_OCV | (fuelgauge->info.enable_topoff_soc << 13);
	pr_info("%s: SM5708_REG_CNTL reg : 0x%x\n", __func__, value);

	ret = sm5708_fg_i2c_write_word(client, SM5708_REG_CNTL, value);
	if (ret < 0)
		pr_info("%s: fail control register set(%d)\n", __func__, ret);

	pr_info("%s: LAST SM5708_REG_CNTL = 0x%x : 0x%x\n", __func__, SM5708_REG_CNTL, value);

	/* LOCK */
	value = SM5708_FG_PARAM_LOCK_CODE | SM5708_FG_TABLE_LEN;
	sm5708_fg_i2c_write_word(client, SM5708_REG_PARAM_CTRL, value);
	pr_info("%s: LAST PARAM CTRL VALUE = 0x%x : 0x%x\n", __func__, SM5708_REG_PARAM_CTRL, value);

	/*	surge reset defence */
	if (is_surge) {
		value = ((fuelgauge->info.batt_ocv<<8)/125);
	} else {
		value = sm5708_calculate_iocv(client);

		if ((fuelgauge->info.volt_cal & 0x0080) == 0x0080) {
			value = value - (fuelgauge->info.volt_cal & 0x007F);
		} else {
			value = value + (fuelgauge->info.volt_cal & 0x007F);
		}
	}

	sm5708_fg_i2c_write_word(client, SM5708_REG_IOCV_MAN, value);
	pr_info("%s: IOCV_MAN_WRITE = %d : 0x%x\n", __func__, SM5708_REG_IOCV_MAN, value);

	/* init delay */
	msleep(20);

	/*	write batt data version */
	value = (fuelgauge->info.data_ver << 4) & SM5708_BATTERY_VERSION;
	sm5708_fg_i2c_write_word(client, SM5708_REG_RESERVED, value);
	pr_info("%s: RESERVED = %d : 0x%x\n", __func__, SM5708_REG_RESERVED, value);

	pr_info("%s: init_MARK = %d : 0x%x\n", __func__, SM5708_REG_RESET, sm5708_fg_i2c_read_word(client, SM5708_REG_RESET));

	return 1;
}

static bool sm5708_fg_init(struct i2c_client *client, bool is_surge)
{
	int ret;
	struct sec_fuelgauge_info *fuelgauge = i2c_get_clientdata(client);

	fuelgauge->info.is_FG_initialised = 0;
	fuelgauge->info.iocv_error_count = 0;

	/* board_fuelgauge_init(fuelgauge); */

	/* SM5708 i2c read check */
	ret = sm5708_get_device_id(client);
	if (ret < 0) {
		pr_info("%s: fail to do i2c read(%d)\n", __func__, ret);
	}

	if (fuelgauge->info.batt_ocv == 0) {
		sm5708_get_ocv(client);
	}

	ret = sm5708_fg_i2c_read_word(client, SM5708_REG_CNTL);
	if (ret != CNTL_REG_DEFAULT_VALUE) {
		fuelgauge->info.cntl_value = ret;
	}

	sm5708_set_soc_cycle_cfg(client);

#if defined(CONFIG_BATTERY_AGE_FORECAST)
	fuelgauge->info.q_max_now = sm5708_fg_i2c_read_word(client, 0xBE);
	pr_info("%s: q_max_now = 0x%x\n", __func__, fuelgauge->info.q_max_now);
	fuelgauge->info.q_max_now = sm5708_fg_i2c_read_word(client, 0xBE);
	pr_info("%s: q_max_now = 0x%x\n", __func__, fuelgauge->info.q_max_now);
#endif

	ret = sm5708_fg_i2c_read_word(client, SM5708_REG_PARAM_CTRL);
	pr_info("%s: SM5708_REG_PARAM_CTRL 0x13 = 0x%x \n", __func__, ret);
	if (ret != (SM5708_FG_PARAM_LOCK_CODE | SM5708_FG_TABLE_LEN)) {
		pr_info("%s: SM5708_FG_PARAM_LOCK_CODE is abnormal Start quick-start\n", __func__);
		// SW reset code
		sm5708_fg_i2c_verified_write_word(client, SM5708_REG_RESET, SW_RESET_CODE);
		// delay 800ms
		msleep(800);
	}
	if (sm5708_fg_check_reg_init_need(client) || (ret != (SM5708_FG_PARAM_LOCK_CODE | SM5708_FG_TABLE_LEN))) {
		sm5708_fg_reg_init(client, is_surge);
	}

	/* curr_off save and restore */
	if (fuelgauge->info.en_auto_curr_offset) {
		ret = sm5708_fg_i2c_read_word(client, SM5708_REG_CURR_OFF);
		fuelgauge->info.curr_offset = ret;
	} else {
		sm5708_fg_i2c_write_word(client, SM5708_REG_CURR_OFF, fuelgauge->info.curr_offset);
	}

	/* set lcal */
	if (fuelgauge->info.curr_lcal_en) {
		sm5708_fg_i2c_write_word(client, SM5708_REG_CURRLCAL_0, fuelgauge->info.curr_lcal_0);
		sm5708_fg_i2c_write_word(client, SM5708_REG_CURRLCAL_1, fuelgauge->info.curr_lcal_1);
		sm5708_fg_i2c_write_word(client, SM5708_REG_CURRLCAL_2, fuelgauge->info.curr_lcal_2);
	}

	/* get first measure all value */
	/* soc */
	sm5708_get_soc(client);
	/* vbat */
	sm5708_get_vbat(client);
	/* current */
	sm5708_get_curr(client);
	/* ocv */
	sm5708_get_ocv(client);
	/* temperature */
	sm5708_get_temperature(client);

	/* cycle */
	sm5708_get_soc_cycle(client);

	pr_info("%s: vbat=%d, vbat_avg=%d, curr=%d, curr_avg=%d, ocv=%d, temp=%d, "
			"cycle=%d, soc=%d, state=0x%x, Q=0x%x\n",
			__func__, fuelgauge->info.batt_voltage, fuelgauge->info.batt_avgvoltage,
			fuelgauge->info.batt_current, fuelgauge->info.batt_avgcurrent, fuelgauge->info.batt_ocv,
			fuelgauge->info.temp_fg, fuelgauge->info.batt_soc_cycle, fuelgauge->info.batt_soc,
			sm5708_fg_i2c_read_word(client, SM5708_REG_OCV_STATE),
			sm5708_fg_i2c_read_word(client, SM5708_REG_Q_EST));

	/* for debug */
	sm5708_fg_buffer_read(client);
	sm5708_fg_test_read(client);

	fuelgauge->info.is_FG_initialised = 1;

	return true;
}

static int sm5708_abnormal_reset_check(struct i2c_client *client)
{
	int cntl_read, reset_read;
	struct sec_fuelgauge_info *fuelgauge = i2c_get_clientdata(client);

	reset_read = sm5708_fg_i2c_read_word(client, SM5708_REG_RESET) & 0xF000;
	/* abnormal case process.... */
	if (sm5708_fg_check_reg_init_need(client) || (reset_read == 0)) {
		cntl_read = sm5708_fg_i2c_read_word(client, SM5708_REG_CNTL);
		pr_info("%s: SM5708 FG abnormal case!!!! SM5708_REG_CNTL : 0x%x, is_FG_initialised : %d, reset_read : 0x%x\n", __func__, cntl_read, fuelgauge->info.is_FG_initialised, reset_read);

		if (fuelgauge->info.is_FG_initialised == 1) {
			/* SW reset code */
			if (sm5708_fg_i2c_verified_write_word(client, SM5708_REG_RESET, SW_RESET_OTP_CODE) < 0) {
				pr_info("%s: Warning!!!! SM5708 FG abnormal case.... SW reset FAIL\n", __func__);
			} else {
				pr_info("%s: SM5708 FG abnormal case.... SW reset OK\n", __func__);
			}
			/* delay 100ms */
			msleep(100);
			/* init code */
			sm5708_fg_init(client, true);
		}

		return FG_ABNORMAL_RESET;
	}
	return 0;
}

#ifdef ENABLE_FULL_OFFSET
void sm5708_adabt_full_offset(struct i2c_client *client)
{
	struct sec_fuelgauge_info *fuelgauge = i2c_get_clientdata(client);
	int fg_temp_gap;
	int full_offset, i_offset, sign_offset, curr;
	int curr_off, sign_origin, i_origin;
	int curr, sign_curr, i_curr;

#ifdef SM5708_FG_FULL_DEBUG
	pr_info("%s: flag_charge_health=%d, flag_full_charge=%d\n", __func__, fuelgauge->info.flag_charge_health, fuelgauge->info.flag_full_charge);
#endif
	if (fuelgauge->info.flag_charge_health && fuelgauge->info.flag_full_charge) {
		fg_temp_gap = (fuelgauge->info.temp_fg/10) - fuelgauge->info.temp_std;
		if (abs(fg_temp_gap) < 10) 	{
			curr = sm5708_fg_i2c_read_word(client, SM5708_REG_CURRENT);
			sign_curr = curr & 0x8000;
			i_curr = (curr & 0x7FFF)>>1;
			if (sign_curr == 1) {
				i_curr = -i_curr;
			}

			curr_off = sm5708_fg_i2c_read_word(client, SM5708_REG_CURR_OFF);
			sign_origin = curr_off & 0x0080;
			i_origin = curr_off & 0x007F;
			if (sign_origin == 1) {
				i_origin = -i_origin;
			}

			full_offset = i_origin - i_curr;
			if (full_offset < 0) {
				i_offset = -full_offset;
				sign_offset = 1;
			} else {
				i_offset = full_offset;
				sign_offset = 0;
			}

			pr_info("%s: curr=%x, curr_off=%x, i_offset=%x, sign_offset=%d, full_offset_margin=%x, full_extra_offset=%x\n", __func__, curr, curr_off, i_offset, sign_offset, fuelgauge->info.full_offset_margin, fuelgauge->info.full_extra_offset);
			if (i_offset < ((fuelgauge->info.full_offset_margin<<10)/1000)) {
				if (sign_offset == 1) {
					i_offset = -i_offset;
				}

				i_offset = i_offset + ((fuelgauge->info.full_extra_offset<<10)/1000);

				if (i_offset <= 0) {
					full_offset = -i_offset;
				} else {
					full_offset = i_offset|0x0080;
				}
				fuelgauge->info.curr_offset = full_offset;
				sm5708_fg_i2c_write_word(client, SM5708_REG_CURR_OFF, full_offset);
				pr_info("%s: LAST i_offset=%x, sign_offset=%x, full_offset=%x\n", __func__, i_offset, sign_offset, full_offset);
			}
		}
	}

	return;
}
#endif

void sm5708_vbatocv_check(struct i2c_client *client)
{
	int ret;
	struct sec_fuelgauge_info *fuelgauge = i2c_get_clientdata(client);

	/* iocv error case cover start */
#ifdef ABSOLUTE_ERROR_OCV_MATCH
	if ((abs(fuelgauge->info.batt_current) < 40) ||
	   ((fuelgauge->is_charging) && (fuelgauge->info.batt_current < (fuelgauge->info.top_off)) &&
	   (fuelgauge->info.batt_current > (fuelgauge->info.top_off/3)) && (fuelgauge->info.batt_soc >= 200)))
#else
	if (((!fuelgauge->ta_exist) && (fuelgauge->info.batt_current < 0) && (fuelgauge->info.batt_current > -40)) ||
		((fuelgauge->ta_exist) && (fuelgauge->info.batt_current > 0) && (fuelgauge->info.batt_current < 40)) ||
		((fuelgauge->is_charging) && (fuelgauge->info.batt_current < (fuelgauge->info.top_off)) &&
		(fuelgauge->info.batt_current > (fuelgauge->info.top_off/3))))
#endif
	{
		if (abs(fuelgauge->info.batt_ocv-fuelgauge->info.batt_voltage) > 30) /* 30mV over */ {
			fuelgauge->info.iocv_error_count++;
		}

		pr_info("%s: sm5708 FG iocv_error_count (%d)\n", __func__, fuelgauge->info.iocv_error_count);

		if (fuelgauge->info.iocv_error_count > 5) /* prevent to overflow */
			fuelgauge->info.iocv_error_count = 6;
	} else {
		fuelgauge->info.iocv_error_count = 0;
	}

	if (fuelgauge->info.iocv_error_count > 5) {
		pr_info("%s: p_v - v = (%d)\n", __func__, fuelgauge->info.p_batt_voltage - fuelgauge->info.batt_voltage);
		if (abs(fuelgauge->info.p_batt_voltage - fuelgauge->info.batt_voltage) > 15) /* 15mV over */ {
			fuelgauge->info.iocv_error_count = 0;
		} else {
			/* mode change to mix RS manual mode */
			pr_info("%s: mode change to mix RS manual mode\n", __func__);
			/* run update set */
			sm5708_fg_i2c_write_word(client, SM5708_REG_PARAM_RUN_UPDATE, 1);
			/* RS manual value write */
			sm5708_fg_i2c_write_word(client, SM5708_REG_RS_MAN, fuelgauge->info.rs_value[0]);
			/* run update set */
			sm5708_fg_i2c_write_word(client, SM5708_REG_PARAM_RUN_UPDATE, 0);
			/* mode change */
			ret = sm5708_fg_i2c_read_word(client, SM5708_REG_CNTL);
			ret = (ret | ENABLE_MIX_MODE) | ENABLE_RS_MAN_MODE; /* +RS_MAN_MODE */
			sm5708_fg_i2c_write_word(client, SM5708_REG_CNTL, ret);
		}
	} else {
		if ((fuelgauge->info.temperature/10) > 15) {
			if ((fuelgauge->info.p_batt_voltage < fuelgauge->info.n_tem_poff) &&
				(fuelgauge->info.batt_voltage < fuelgauge->info.n_tem_poff) && (!fuelgauge->is_charging)) {
				pr_info("%s: mode change to normal tem mix RS manual mode\n", __func__);
				/* mode change to mix RS manual mode */
				/* run update init */
				sm5708_fg_i2c_write_word(client, SM5708_REG_PARAM_RUN_UPDATE, 1);
				/* RS manual value write */
				if ((fuelgauge->info.p_batt_voltage <
					(fuelgauge->info.n_tem_poff - fuelgauge->info.n_tem_poff_offset)) &&
					(fuelgauge->info.batt_voltage <
					(fuelgauge->info.n_tem_poff - fuelgauge->info.n_tem_poff_offset))) {
					sm5708_fg_i2c_write_word(client, SM5708_REG_RS_MAN, fuelgauge->info.rs_value[0]>>1);
				} else {
					sm5708_fg_i2c_write_word(client, SM5708_REG_RS_MAN, fuelgauge->info.rs_value[0]);
				}
				/* run update set */
				sm5708_fg_i2c_write_word(client, SM5708_REG_PARAM_RUN_UPDATE, 0);

				/* mode change */
				ret = sm5708_fg_i2c_read_word(client, SM5708_REG_CNTL);
				ret = (ret | ENABLE_MIX_MODE) | ENABLE_RS_MAN_MODE; /* +RS_MAN_MODE */
				sm5708_fg_i2c_write_word(client, SM5708_REG_CNTL, ret);
			} else {
				pr_info("%s: mode change to mix RS auto mode\n", __func__);

				/* mode change to mix RS auto mode */
				ret = sm5708_fg_i2c_read_word(client, SM5708_REG_CNTL);
				ret = (ret | ENABLE_MIX_MODE) & ~ENABLE_RS_MAN_MODE; /* -RS_MAN_MODE */
				sm5708_fg_i2c_write_word(client, SM5708_REG_CNTL, ret);
			}
		} else {
			if ((fuelgauge->info.p_batt_voltage < fuelgauge->info.l_tem_poff) &&
				(fuelgauge->info.batt_voltage < fuelgauge->info.l_tem_poff) && (!fuelgauge->is_charging)) {
				pr_info("%s: mode change to normal tem mix RS manual mode\n", __func__);
				/* mode change to mix RS manual mode */
				/*	run update init */
				sm5708_fg_i2c_write_word(client, SM5708_REG_PARAM_RUN_UPDATE, 1);
				/* RS manual value write */
				if ((fuelgauge->info.p_batt_voltage <
					(fuelgauge->info.l_tem_poff - fuelgauge->info.l_tem_poff_offset)) &&
					(fuelgauge->info.batt_voltage <
					(fuelgauge->info.l_tem_poff - fuelgauge->info.l_tem_poff_offset))) {
					sm5708_fg_i2c_write_word(client, SM5708_REG_RS_MAN, fuelgauge->info.rs_value[0]>>1);
				} else {
					sm5708_fg_i2c_write_word(client, SM5708_REG_RS_MAN, fuelgauge->info.rs_value[0]);
				}
				/* run update set */
				sm5708_fg_i2c_write_word(client, SM5708_REG_PARAM_RUN_UPDATE, 0);

				/* mode change */
				ret = sm5708_fg_i2c_read_word(client, SM5708_REG_CNTL);
				ret = (ret | ENABLE_MIX_MODE) | ENABLE_RS_MAN_MODE; /* +RS_MAN_MODE */
				sm5708_fg_i2c_write_word(client, SM5708_REG_CNTL, ret);
			} else {
				pr_info("%s: mode change to mix RS auto mode\n", __func__);

				/* mode change to mix RS auto mode */
				ret = sm5708_fg_i2c_read_word(client, SM5708_REG_CNTL);
				ret = (ret | ENABLE_MIX_MODE) & ~ENABLE_RS_MAN_MODE; /* -RS_MAN_MODE */
				sm5708_fg_i2c_write_word(client, SM5708_REG_CNTL, ret);
			}
		}
	}
	fuelgauge->info.p_batt_voltage = fuelgauge->info.batt_voltage;
	fuelgauge->info.p_batt_current = fuelgauge->info.batt_current;
	/* iocv error case cover end */

	/* this code is for 0x14 sitll has god 1 */
	ret = sm5708_fg_i2c_read_word(client, SM5708_REG_PARAM_RUN_UPDATE);
	pr_info("%s: PARAM_RUN_UPDATE 0x14 = 0x%x \n", __func__, ret);
	if(ret) {
		pr_info("%s: force update PARAM_RUN_UPDATE \n", __func__);
		/* run update set */
		sm5708_fg_i2c_write_word(client, SM5708_REG_PARAM_RUN_UPDATE, 0);
	}	
}

static int sm5708_cal_carc (struct i2c_client *client)
{
	int p_curr_cal = 0, n_curr_cal = 0, p_delta_cal = 0, n_delta_cal = 0, p_fg_delta_cal = 0, n_fg_delta_cal = 0, temp_curr_offset = 0;
	int volt_cal = 0, fg_delta_volcal = 0, pn_volt_slope = 0, volt_offset = 0;
	int temp_gap, fg_temp_gap, mix_factor = 0;

	struct sec_fuelgauge_info *fuelgauge = i2c_get_clientdata(client);

	sm5708_vbatocv_check(client);
#ifdef ENABLE_FULL_OFFSET
	sm5708_adabt_full_offset(client);
#endif

	if (fuelgauge->is_charging || (fuelgauge->info.batt_current < LIMIT_N_CURR_MIXFACTOR)) {
		mix_factor = fuelgauge->info.rs_value[1];
	} else {
		mix_factor = fuelgauge->info.rs_value[2];
	}
	sm5708_fg_i2c_write_word(client, SM5708_REG_RS_MIX_FACTOR, mix_factor);

	fg_temp_gap = (fuelgauge->info.temp_fg/10) - fuelgauge->info.temp_std;

	volt_cal = sm5708_fg_i2c_read_word(client, SM5708_REG_VOLT_CAL);
	volt_offset = volt_cal & 0x00FF;
	pn_volt_slope = fuelgauge->info.volt_cal & 0xFF00;

	if (fuelgauge->info.en_fg_temp_volcal) {
		fg_delta_volcal = (fg_temp_gap / fuelgauge->info.fg_temp_volcal_denom)*fuelgauge->info.fg_temp_volcal_fact;
		pn_volt_slope = pn_volt_slope + (fg_delta_volcal<<8);
		volt_cal = pn_volt_slope | volt_offset;
		sm5708_fg_i2c_write_word(client, SM5708_REG_VOLT_CAL, volt_cal);
	}

	temp_curr_offset = fuelgauge->info.curr_offset;
	if (fuelgauge->info.en_high_fg_temp_offset && (fg_temp_gap > 0)) {
		if (temp_curr_offset & 0x0080) {
			temp_curr_offset = -(temp_curr_offset & 0x007F);
		}
		temp_curr_offset = temp_curr_offset + (fg_temp_gap / fuelgauge->info.high_fg_temp_offset_denom)*fuelgauge->info.high_fg_temp_offset_fact;
		if (temp_curr_offset < 0) {
			temp_curr_offset = -temp_curr_offset;
			temp_curr_offset = temp_curr_offset|0x0080;
		}
	} else if (fuelgauge->info.en_low_fg_temp_offset && (fg_temp_gap < 0)) {
		if (temp_curr_offset & 0x0080) {
			temp_curr_offset = -(temp_curr_offset & 0x007F);
		}
		temp_curr_offset = temp_curr_offset + ((-fg_temp_gap) / fuelgauge->info.low_fg_temp_offset_denom)*fuelgauge->info.low_fg_temp_offset_fact;
		if (temp_curr_offset < 0) {
			temp_curr_offset = -temp_curr_offset;
			temp_curr_offset = temp_curr_offset|0x0080;
		}
	}
	sm5708_fg_i2c_write_word(client, SM5708_REG_CURR_OFF, temp_curr_offset);

	n_curr_cal = fuelgauge->info.n_curr_cal;
	p_curr_cal = fuelgauge->info.p_curr_cal;

	if (fuelgauge->info.en_high_fg_temp_cal && (fg_temp_gap > 0)) {
		p_fg_delta_cal = (fg_temp_gap / fuelgauge->info.high_fg_temp_p_cal_denom)*fuelgauge->info.high_fg_temp_p_cal_fact;
		n_fg_delta_cal = (fg_temp_gap / fuelgauge->info.high_fg_temp_n_cal_denom)*fuelgauge->info.high_fg_temp_n_cal_fact;
	} else if (fuelgauge->info.en_low_fg_temp_cal && (fg_temp_gap < 0)) {
		fg_temp_gap = -fg_temp_gap;
		p_fg_delta_cal = (fg_temp_gap / fuelgauge->info.low_fg_temp_p_cal_denom)*fuelgauge->info.low_fg_temp_p_cal_fact;
		n_fg_delta_cal = (fg_temp_gap / fuelgauge->info.low_fg_temp_n_cal_denom)*fuelgauge->info.low_fg_temp_n_cal_fact;
	}
	p_curr_cal = p_curr_cal + (p_fg_delta_cal);
	n_curr_cal = n_curr_cal + (n_fg_delta_cal);

	pr_info("%s: <%d %d %d %d %d %d %d %d %d %d>, temp_fg = %d ,p_curr_cal = 0x%x, n_curr_cal = 0x%x, "
		"curr_offset = 0x%x, volt_cal = 0x%x ,fg_delta_volcal = 0x%x\n",
		__func__,
		fuelgauge->info.en_high_fg_temp_cal,
		fuelgauge->info.high_fg_temp_p_cal_denom, fuelgauge->info.high_fg_temp_p_cal_fact,
		fuelgauge->info.high_fg_temp_n_cal_denom, fuelgauge->info.high_fg_temp_n_cal_fact,
		fuelgauge->info.en_low_fg_temp_cal,
		fuelgauge->info.low_fg_temp_p_cal_denom, fuelgauge->info.low_fg_temp_p_cal_fact,
		fuelgauge->info.low_fg_temp_n_cal_denom, fuelgauge->info.low_fg_temp_n_cal_fact,
		fuelgauge->info.temp_fg, p_curr_cal, n_curr_cal, temp_curr_offset,
		volt_cal, fg_delta_volcal);

	temp_gap = (fuelgauge->info.temperature/10) - fuelgauge->info.temp_std;
	if (fuelgauge->info.en_high_temp_cal && (temp_gap > 0)) {
		p_delta_cal = (temp_gap / fuelgauge->info.high_temp_p_cal_denom)*fuelgauge->info.high_temp_p_cal_fact;
		n_delta_cal = (temp_gap / fuelgauge->info.high_temp_n_cal_denom)*fuelgauge->info.high_temp_n_cal_fact;
	} else if (fuelgauge->info.en_low_temp_cal && (temp_gap < 0)) {
		temp_gap = -temp_gap;
		p_delta_cal = (temp_gap / fuelgauge->info.low_temp_p_cal_denom)*fuelgauge->info.low_temp_p_cal_fact;
		n_delta_cal = (temp_gap / fuelgauge->info.low_temp_n_cal_denom)*fuelgauge->info.low_temp_n_cal_fact;
	}
	p_curr_cal = p_curr_cal + (p_delta_cal);
	n_curr_cal = n_curr_cal + (n_delta_cal);

	sm5708_fg_i2c_write_word(client, SM5708_REG_CURR_P_SLOPE, p_curr_cal);
	sm5708_fg_i2c_write_word(client, SM5708_REG_CURR_N_SLOPE, n_curr_cal);

	pr_info("%s: <%d %d %d %d %d %d %d %d %d %d>, "
		"p_curr_cal = 0x%x, n_curr_cal = 0x%x, mix_factor=0x%x ,batt_temp = %d\n",
		__func__,
		fuelgauge->info.en_high_temp_cal,
		fuelgauge->info.high_temp_p_cal_denom, fuelgauge->info.high_temp_p_cal_fact,
		fuelgauge->info.high_temp_n_cal_denom, fuelgauge->info.high_temp_n_cal_fact,
		fuelgauge->info.en_low_temp_cal,
		fuelgauge->info.low_temp_p_cal_denom, fuelgauge->info.low_temp_p_cal_fact,
		fuelgauge->info.low_temp_n_cal_denom, fuelgauge->info.low_temp_n_cal_fact,
		p_curr_cal, n_curr_cal, mix_factor, fuelgauge->info.temperature);

	return 0;
}

static int sm5708_get_all_value(struct i2c_client *client)
{
	union power_supply_propval value;
	struct sec_fuelgauge_info *fuelgauge = i2c_get_clientdata(client);

	/* check charging. */
	value.intval = POWER_SUPPLY_HEALTH_UNKNOWN;
	psy_do_property("sm5708-charger", get,
			POWER_SUPPLY_PROP_HEALTH, value);
#ifdef SM5708_FG_FULL_DEBUG
	pr_info("%s: get POWER_SUPPLY_PROP_HEALTH = 0x%x\n", __func__, value.intval);
#endif
	fuelgauge->info.flag_charge_health =
		(value.intval == POWER_SUPPLY_HEALTH_GOOD) ? 1 : 0;

	fuelgauge->is_charging = (fuelgauge->info.flag_charge_health |
		fuelgauge->ta_exist) && (fuelgauge->info.batt_current >= 30);

	/* check charger status */
	psy_do_property("sm5708-charger", get,
			POWER_SUPPLY_PROP_STATUS, value);
	fuelgauge->info.flag_full_charge =
		(value.intval == POWER_SUPPLY_STATUS_FULL) ? 1 : 0;
	fuelgauge->info.flag_chg_status =
		(value.intval == POWER_SUPPLY_STATUS_CHARGING) ? 1 : 0;

	/* vbat */
	sm5708_get_vbat(client);
	/* current */
	sm5708_get_curr(client);
	/* ocv */
	sm5708_get_ocv(client);
	/* temperature */
	sm5708_get_temperature(client);
	/*cycle */
	sm5708_get_soc_cycle(client);
	/* carc */
	sm5708_cal_carc(client);
	/* soc */
	sm5708_get_soc(client);

	sm5708_fg_test_read(client);

	pr_info("%s: chg_h=%d, chg_f=%d, chg_s=%d, is_chg=%d, ta_exist=%d, "
		"v=%d, v_avg=%d, i=%d, i_avg=%d, ocv=%d, fg_t=%d, b_t=%d, cycle=%d, soc=%d, state=0x%x\n",
		__func__, fuelgauge->info.flag_charge_health, fuelgauge->info.flag_full_charge,
		fuelgauge->info.flag_chg_status, fuelgauge->is_charging, fuelgauge->ta_exist,
		fuelgauge->info.batt_voltage, fuelgauge->info.batt_avgvoltage,
		fuelgauge->info.batt_current, fuelgauge->info.batt_avgcurrent, fuelgauge->info.batt_ocv,
		fuelgauge->info.temp_fg, fuelgauge->info.temperature, fuelgauge->info.batt_soc_cycle,
		fuelgauge->info.batt_soc, sm5708_fg_i2c_read_word(client, SM5708_REG_OCV_STATE));

	return 0;
}

static int sm5708_fg_get_jig_mode_real_vbat(struct i2c_client *client)
{
	int cntl, ret;

	cntl = sm5708_fg_i2c_read_word(client, SM5708_REG_CNTL);
	pr_info("%s: start, CNTL=0x%x\n", __func__, cntl);

	if (sm5708_fg_check_reg_init_need(client)) {
		return -EINVAL; /* -1 */
	}

	cntl = cntl | ENABLE_MODE_nENQ4;
	sm5708_fg_i2c_write_word(client, SM5708_REG_CNTL, cntl);

	msleep(300);

	ret = sm5708_get_vbat(client);
	pr_info("%s: jig mode real batt V = %d, CNTL=0x%x\n", __func__, ret, cntl);

	cntl = sm5708_fg_i2c_read_word(client, SM5708_REG_CNTL);
	cntl = cntl & (~ENABLE_MODE_nENQ4);
	sm5708_fg_i2c_write_word(client, SM5708_REG_CNTL, cntl);

	pr_info("%s: end_1, CNTL=0x%x\n", __func__, cntl);
	msleep(300);

	cntl = sm5708_fg_i2c_read_word(client, SM5708_REG_CNTL);
	cntl = cntl & (~ENABLE_MODE_nENQ4);
	sm5708_fg_i2c_write_word(client, SM5708_REG_CNTL, cntl);

	pr_info("%s: end_2, CNTL=0x%x\n", __func__, cntl);

	return ret;
}

#ifdef CONFIG_OF
#if defined(CONFIG_PROJECT_GTS210VE)

#define SDI_ADC_MAX_LIMIT 30000
static struct qpnp_vadc_chip *adc_client;
static enum qpnp_vadc_channels batt_id_adc_channel;

static void sm5708_adc_ap_init(struct sec_fuelgauge_info *fuelgauge)
{

		if (!(&fuelgauge->client->dev)) {
				pr_err("%s : can't get fuelgauge dev\n", __func__);
		} else {
				adc_client = qpnp_get_vadc(&fuelgauge->client->dev, "sm5708-fuelgauge");
				if (IS_ERR(adc_client)) {
						int rc;
						rc = PTR_ERR(adc_client);
						if (rc != -EPROBE_DEFER)
								pr_err("%s: Fail to get vadc %d\n", __func__, rc);
				}
		}
}

static int sm5708_adc_ap_read(int channel)
{
	struct qpnp_vadc_result result;
	int data = -1;
	int rc;

	switch (channel) {
	case SEC_BAT_ADC_CHANNEL_BAT_CHECK:
		rc = qpnp_vadc_read(adc_client, batt_id_adc_channel, &result);

		if (rc) {
			pr_err("%s: Unable to read batt adc=%d, batt_id_adc_channel=%d\n",
				__func__, rc, batt_id_adc_channel);
			return 0;
		}
		data = result.adc_code;
		break;
	default:
		break;
	}

	pr_debug("%s: data(%d)\n", __func__, data);

	return data;
}

static int get_battery_id(struct sec_fuelgauge_info *fuelgauge, enum sec_battery_adc_channel channel)
{
	int batt_adc;
	fuelgauge->info.battery_typ = SDI_BATTERY_TYPE;

	batt_id_adc_channel = P_MUX2_1_1;

	pr_info("%s channel = %d\n", __func__, channel);

	if (channel == SEC_BAT_ADC_CHANNEL_BAT_CHECK) {
		sec_mpp_mux_control(BATT_ID_MUX_SEL_NUM, SEC_MUX_SEL_BATT_ID, 1);
		batt_adc = sm5708_adc_ap_read(channel);
		sec_mpp_mux_control(BATT_ID_MUX_SEL_NUM, SEC_MUX_SEL_BATT_ID, 0);

	if (batt_adc > SDI_ADC_MAX_LIMIT) {
		fuelgauge->info.battery_typ = ATL_BATTERY_TYPE;
		pr_info("%s: batt_id_adc = (%d), battery type (%d)\n", __func__, batt_adc, fuelgauge->info.battery_typ);
		return ATL_BATTERY_TYPE;
	} else {
		fuelgauge->info.battery_typ = SDI_BATTERY_TYPE;
		pr_info("%s: batt_id_adc = (%d), battery type (%d)\n", __func__, batt_adc, fuelgauge->info.battery_typ);
		return SDI_BATTERY_TYPE;
	}

	pr_info("%s : ADC not in range batt_id_adc = (%d)\n", __func__, batt_adc);

	}
	return SDI_BATTERY_TYPE;
}
#else
static int get_battery_id(struct sec_fuelgauge_info *fuelgauge)
{
	/* sm5708fg does not support this function */
	return 0;
}
#endif
#define PROPERTY_NAME_SIZE 128

#define PINFO(format, args...) \
	printk(KERN_INFO "%s() line-%d: " format, \
		__func__, __LINE__, ## args)

#define DECL_PARAM_PROP(_id, _name) {.id = _id, .name = _name,}

#if defined(CONFIG_BATTERY_AGE_FORECAST)
static int temp_parse_dt(struct sec_fuelgauge_info *fuelgauge)
{
	struct device_node *np = of_find_node_by_name(NULL, "battery");
	int len = 0, ret;
	const u32 *p;

	if (np == NULL) {
		pr_err("%s np NULL\n", __func__);
	} else {
		p = of_get_property(np, "battery,age_data", &len);
		if (p) {
			fuelgauge->pdata->num_age_step = len / sizeof(sec_age_data_t);
			fuelgauge->pdata->age_data = kzalloc(len, GFP_KERNEL);
			ret = of_property_read_u32_array(np, "battery,age_data",
					 (u32 *)fuelgauge->pdata->age_data, len/sizeof(u32));
			if (ret) {
				pr_err("%s failed to read battery->pdata->age_data: %d\n",
						__func__, ret);
				kfree(fuelgauge->pdata->age_data);
				fuelgauge->pdata->age_data = NULL;
				fuelgauge->pdata->num_age_step = 0;
			}
			pr_info("%s num_age_step : %d\n", __func__, fuelgauge->pdata->num_age_step);
			for (len = 0; len < fuelgauge->pdata->num_age_step; ++len) {
				pr_info("[%d/%d]cycle:%d, float:%d, full_v:%d, recharge_v:%d, soc:%d\n",
					len, fuelgauge->pdata->num_age_step-1,
					fuelgauge->pdata->age_data[len].cycle,
					fuelgauge->pdata->age_data[len].float_voltage,
					fuelgauge->pdata->age_data[len].full_condition_vcell,
					fuelgauge->pdata->age_data[len].recharge_condition_vcell,
					fuelgauge->pdata->age_data[len].full_condition_soc);
			}
		} else {
			fuelgauge->pdata->num_age_step = 0;
			pr_err("%s there is not age_data\n", __func__);
		}
	}
	return 0;
}
#endif

static int sm5708_fg_parse_dt(struct sec_fuelgauge_info *fuelgauge)
{
	char prop_name[PROPERTY_NAME_SIZE];
	int battery_id = -1;
#if defined(CONFIG_BATTERY_AGE_FORECAST)
	int v_max_table[5];
	int q_max_table[5];
#endif
	int table[16];
	int rce_value[3];
	int rs_value[5];
	int mix_value[2];
	int topoff_soc[3];
	int cycle_cfg[3];
	int v_offset_cancel[4];
	int temp_volcal[3];
	int temp_offset[6];
	int temp_cal[10];
	int ext_temp_cal[10];
	int set_temp_poff[4];
	int curr_offset[2];
	int curr_lcal[4];
#ifdef ENABLE_FULL_OFFSET
	int full_offset[2];
#endif

	int ret;
	int i, j;

	struct device_node *np = of_find_node_by_name(NULL, "sm5708-fuelgauge");

	/* reset, irq gpio info */
	if (np == NULL) {
		pr_err("%s np NULL\n", __func__);
	} else {
		ret = of_get_named_gpio(np, "fuelgauge,fuel_int", 0);
		if (ret > 0) {
			fuelgauge->pdata->fg_irq = ret;
			pr_info("%s reading fg_irq = %d\n", __func__, ret);
		}

		ret = of_get_named_gpio(np, "fuelgauge,bat_int", 0);
		if (ret > 0) {
			fuelgauge->pdata->bat_irq_gpio = ret;
			fuelgauge->pdata->bat_irq = gpio_to_irq(ret);
			pr_info("%s reading bat_int_gpio = %d\n", __func__, ret);
		}

		ret = of_property_read_u32(np, "fuelgauge,capacity_max",
				&fuelgauge->pdata->capacity_max);
		if (ret < 0)
			pr_err("%s error reading capacity_max %d\n", __func__, ret);

		ret = of_property_read_u32(np, "fuelgauge,capacity_max_margin",
				&fuelgauge->pdata->capacity_max_margin);
		if (ret < 0)
			pr_err("%s error reading capacity_max_margin %d\n", __func__, ret);

		ret = of_property_read_u32(np, "fuelgauge,capacity_min",
				&fuelgauge->pdata->capacity_min);
		if (ret < 0)
			pr_err("%s error reading capacity_min %d\n", __func__, ret);

		pr_info("%s: capacity_max: %d, "
				"capacity_max_margin: 0x%x, "
				"capacity_min: %d\n", __func__, fuelgauge->pdata->capacity_max,
				fuelgauge->pdata->capacity_max_margin,
				fuelgauge->pdata->capacity_min);

		ret = of_property_read_u32(np, "fuelgauge,capacity_calculation_type",
				&fuelgauge->pdata->capacity_calculation_type);
		if (ret < 0)
			pr_err("%s error reading capacity_calculation_type %d\n",
					__func__, ret);
		ret = of_property_read_u32(np, "fuelgauge,fuel_alert_soc",
				&fuelgauge->pdata->fuel_alert_soc);
		if (ret < 0)
			pr_err("%s error reading pdata->fuel_alert_soc %d\n",
					__func__, ret);
		fuelgauge->pdata->repeated_fuelalert = of_property_read_bool(np,
				"fuelgaguge,repeated_fuelalert");

		pr_info("%s: fg_irq: %d, "
				"calculation_type: 0x%x, fuel_alert_soc: %d,\n"
				"repeated_fuelalert: %d\n", __func__, fuelgauge->pdata->fg_irq,
				fuelgauge->pdata->capacity_calculation_type,
				fuelgauge->pdata->fuel_alert_soc, fuelgauge->pdata->repeated_fuelalert);
	}

	/* get battery_params node */
	np = of_find_node_by_name(of_node_get(np), "battery_params");
	if (np == NULL) {
		PINFO("Cannot find child node \"battery_params\"\n");
		return -EINVAL;
	}

#if defined(CONFIG_PROJECT_GTS210VE)
	/*To initialize batt_id_adc channel*/
	sm5708_adc_ap_init(fuelgauge);
#endif

	/* get battery_id */
	if (of_property_read_u32(np, "battery,id", &battery_id) < 0)
		PINFO("not battery,id property\n");
	if (battery_id == -1)
#if defined(CONFIG_PROJECT_GTS210VE)
		battery_id = get_battery_id(fuelgauge, SEC_BAT_ADC_CHANNEL_BAT_CHECK);
#else
		battery_id = get_battery_id(fuelgauge);
#endif
	PINFO("battery id = %d\n", battery_id);

#if defined(CONFIG_BATTERY_AGE_FORECAST)
	snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s", battery_id, "v_max_table");
	ret = of_property_read_u32_array(np, prop_name, v_max_table, fuelgauge->pdata->num_age_step);

	if (ret < 0) {
		PINFO("Can get prop %s (%d)\n", prop_name, ret);

		for (i = 0; i < fuelgauge->pdata->num_age_step; i++) {
			fuelgauge->info.v_max_table[i] = fuelgauge->info.battery_table[DISCHARGE_TABLE][SM5708_FG_TABLE_LEN-1];
			PINFO("%s = <v_max_table[%d] 0x%x>\n", prop_name, i, fuelgauge->info.v_max_table[i]);
		}
	} else {
		for (i = 0; i < fuelgauge->pdata->num_age_step; i++) {
			fuelgauge->info.v_max_table[i] = v_max_table[i];
			PINFO("%s = <v_max_table[%d] 0x%x>\n", prop_name, i, fuelgauge->info.v_max_table[i]);
		}
	}

	snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s", battery_id, "q_max_table");
	ret = of_property_read_u32_array(np, prop_name, q_max_table, fuelgauge->pdata->num_age_step);

	if (ret < 0) {
		PINFO("Can get prop %s (%d)\n", prop_name, ret);

		for (i = 0; i < fuelgauge->pdata->num_age_step; i++) {
			fuelgauge->info.q_max_table[i] = 100;
			PINFO("%s = <q_max_table[%d] %d>\n", prop_name, i, fuelgauge->info.q_max_table[i]);
		}
	} else {
		for (i = 0; i < fuelgauge->pdata->num_age_step; i++) {
			fuelgauge->info.q_max_table[i] = q_max_table[i];
			PINFO("%s = <q_max_table[%d] %d>\n", prop_name, i, fuelgauge->info.q_max_table[i]);
		}
	}
	fuelgauge->chg_full_soc = fuelgauge->pdata->age_data[0].full_condition_soc;
	fuelgauge->info.v_max_now = fuelgauge->info.v_max_table[0];
	fuelgauge->info.q_max_now = fuelgauge->info.q_max_table[0];
	PINFO("%s = <v_max_now = 0x%x>, <q_max_now = 0x%x>, <chg_full_soc = %d>\n", prop_name, fuelgauge->info.v_max_now, fuelgauge->info.q_max_now, fuelgauge->chg_full_soc);
#endif

	// get battery_table
	for (i = DISCHARGE_TABLE; i < TABLE_MAX; i++) {
		snprintf(prop_name, PROPERTY_NAME_SIZE,
			 "battery%d,%s%d", battery_id, "battery_table", i);

		ret = of_property_read_u32_array(np, prop_name, table, 16);
		if (ret < 0) {
			PINFO("Can get prop %s (%d)\n", prop_name, ret);
		}
		for (j = 0; j <= SM5708_FG_TABLE_LEN; j++) {
			fuelgauge->info.battery_table[i][j] = table[j];
			PINFO("%s = <table[%d][%d] 0x%x>\n", prop_name, i, j, table[j]);
		}
	}

	/* get rce */
	for (i = 0; i < 3; i++) {
		snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s", battery_id, "rce_value");
		ret = of_property_read_u32_array(np, prop_name, rce_value, 3);
		if (ret < 0) {
			PINFO("Can get prop %s (%d)\n", prop_name, ret);
		}
		fuelgauge->info.rce_value[i] = rce_value[i];
	}
	PINFO("%s = <0x%x 0x%x 0x%x>\n", prop_name, rce_value[0], rce_value[1], rce_value[2]);

	/* get dtcd_value */
	snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s", battery_id, "dtcd_value");
	ret = of_property_read_u32_array(np, prop_name, &fuelgauge->info.dtcd_value, 1);
	if (ret < 0)
		PINFO("Can get prop %s (%d)\n", prop_name, ret);
	PINFO("%s = <0x%x>\n", prop_name, fuelgauge->info.dtcd_value);

	/* get rs_value */
	for (i = 0; i < 5; i++) {
		snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s", battery_id, "rs_value");
		ret = of_property_read_u32_array(np, prop_name, rs_value, 5);
		if (ret < 0) {
			PINFO("Can get prop %s (%d)\n", prop_name, ret);
		}
		fuelgauge->info.rs_value[i] = rs_value[i];
	}
	PINFO("%s = <0x%x 0x%x 0x%x 0x%x 0x%x>\n", prop_name, rs_value[0], rs_value[1], rs_value[2], rs_value[3], rs_value[4]);

	/* get vit_period */
	snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s", battery_id, "vit_period");
	ret = of_property_read_u32_array(np, prop_name, &fuelgauge->info.vit_period, 1);
	if (ret < 0)
		PINFO("Can get prop %s (%d)\n", prop_name, ret);
	PINFO("%s = <0x%x>\n", prop_name, fuelgauge->info.vit_period);

	/* get mix_value */
	for (i = 0; i < 2; i++) {
		snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s", battery_id, "mix_value");
		ret = of_property_read_u32_array(np, prop_name, mix_value, 2);
		if (ret < 0) {
			PINFO("Can get prop %s (%d)\n", prop_name, ret);
		}
		fuelgauge->info.mix_value[i] = mix_value[i];
	}
	PINFO("%s = <0x%x 0x%x>\n", prop_name, mix_value[0], mix_value[1]);

	/* battery_type */
	snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s", battery_id, "battery_type");
	ret = of_property_read_u32_array(np, prop_name, &fuelgauge->info.battery_type, 1);
	if (ret < 0)
		PINFO("Can get prop %s (%d)\n", prop_name, ret);
	PINFO("%s = <%d>\n", prop_name, fuelgauge->info.battery_type);

	/* MISC */
	snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s", battery_id, "misc");
	ret = of_property_read_u32_array(np, prop_name, &fuelgauge->info.misc, 1);
	if (ret < 0)
		PINFO("Can get prop %s (%d)\n", prop_name, ret);
	PINFO("%s = <0x%x>\n", prop_name, fuelgauge->info.misc);

	/* V_ALARM */
	snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s", battery_id, "v_alarm");
	ret = of_property_read_u32_array(np, prop_name, &fuelgauge->info.value_v_alarm, 1);
	if (ret < 0)
		PINFO("Can get prop %s (%d)\n", prop_name, ret);
	PINFO("%s = <%d>\n", prop_name, fuelgauge->info.value_v_alarm);

	/* TOP OFF SOC */
	snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s", battery_id, "topoff_soc");
	ret = of_property_read_u32_array(np, prop_name, topoff_soc, 3);
	if (ret < 0)
		PINFO("Can get prop %s (%d)\n", prop_name, ret);
	fuelgauge->info.enable_topoff_soc = topoff_soc[0];
	fuelgauge->info.topoff_soc = topoff_soc[1];
	fuelgauge->info.top_off = topoff_soc[2];

	PINFO("%s = <%d %d %d>\n", prop_name,
		fuelgauge->info.enable_topoff_soc, fuelgauge->info.topoff_soc, fuelgauge->info.top_off);

	/* SOC cycle cfg */
	snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s", battery_id, "cycle_cfg");
	ret = of_property_read_u32_array(np, prop_name, cycle_cfg, 3);
	if (ret < 0)
		PINFO("Can get prop %s (%d)\n", prop_name, ret);
	fuelgauge->info.cycle_high_limit = cycle_cfg[0];
	fuelgauge->info.cycle_low_limit = cycle_cfg[1];
	fuelgauge->info.cycle_limit_cntl = cycle_cfg[2];

	PINFO("%s = <%d %d %d>\n", prop_name,
		fuelgauge->info.cycle_high_limit, fuelgauge->info.cycle_low_limit, fuelgauge->info.cycle_limit_cntl);

	/* v_offset_cancel */
	snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s", battery_id, "v_offset_cancel");
	ret = of_property_read_u32_array(np, prop_name, v_offset_cancel, 4);
	if (ret < 0)
		PINFO("Can get prop %s (%d)\n", prop_name, ret);
	fuelgauge->info.enable_v_offset_cancel_p = v_offset_cancel[0];
	fuelgauge->info.enable_v_offset_cancel_n = v_offset_cancel[1];
	fuelgauge->info.v_offset_cancel_level = v_offset_cancel[2];
	fuelgauge->info.v_offset_cancel_mohm = v_offset_cancel[3];

	PINFO("%s = <%d %d %d %d>\n", prop_name,
		fuelgauge->info.enable_v_offset_cancel_p, fuelgauge->info.enable_v_offset_cancel_n, fuelgauge->info.v_offset_cancel_level, fuelgauge->info.v_offset_cancel_mohm);

	/* VOL & CURR CAL */
	snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s", battery_id, "volt_cal");
	ret = of_property_read_u32_array(np, prop_name, &fuelgauge->info.volt_cal, 1);
	if (ret < 0)
		PINFO("Can get prop %s (%d)\n", prop_name, ret);
	PINFO("%s = <0x%x>\n", prop_name, fuelgauge->info.volt_cal);

	snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s", battery_id, "curr_offset");
	ret = of_property_read_u32_array(np, prop_name, curr_offset, 2);
	if (ret < 0)
		PINFO("Can get prop %s (%d)\n", prop_name, ret);
	fuelgauge->info.en_auto_curr_offset = curr_offset[0];
	fuelgauge->info.curr_offset = curr_offset[1];

	PINFO("%s = <%d 0x%x>\n", prop_name, fuelgauge->info.en_auto_curr_offset, fuelgauge->info.curr_offset);

#ifdef ENABLE_FULL_OFFSET
	snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s", battery_id, "full_offset");
	ret = of_property_read_u32_array(np, prop_name, full_offset, 2);
	if (ret < 0)
		PINFO("Can get prop %s (%d)\n", prop_name, ret);
	fuelgauge->info.full_offset_margin = full_offset[0];
	fuelgauge->info.full_extra_offset = full_offset[1];

	PINFO("%s = <%d %d>\n", prop_name, fuelgauge->info.full_offset_margin, fuelgauge->info.full_extra_offset);
#endif

	snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s", battery_id, "p_curr_cal");
	ret = of_property_read_u32_array(np, prop_name, &fuelgauge->info.p_curr_cal, 1);
	if (ret < 0)
		PINFO("Can get prop %s (%d)\n", prop_name, ret);
	PINFO("%s = <0x%x>\n", prop_name, fuelgauge->info.p_curr_cal);

	snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s", battery_id, "n_curr_cal");
	ret = of_property_read_u32_array(np, prop_name, &fuelgauge->info.n_curr_cal, 1);
	if (ret < 0)
		PINFO("Can get prop %s (%d)\n", prop_name, ret);
	PINFO("%s = <0x%x>\n", prop_name, fuelgauge->info.n_curr_cal);

	/* curr_lcal */
	snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s", battery_id, "curr_lcal");
	ret = of_property_read_u32_array(np, prop_name, curr_lcal, 4);
	if (ret < 0)
		PINFO("Can get prop %s (%d)\n", prop_name, ret);
	fuelgauge->info.curr_lcal_en = curr_lcal[0];
	fuelgauge->info.curr_lcal_0 = curr_lcal[1];
	fuelgauge->info.curr_lcal_1 = curr_lcal[2];
	fuelgauge->info.curr_lcal_2 = curr_lcal[3];
	PINFO("%s = <%d, 0x%x, 0x%x, 0x%x>\n", prop_name,
		fuelgauge->info.curr_lcal_en, fuelgauge->info.curr_lcal_0, fuelgauge->info.curr_lcal_1, fuelgauge->info.curr_lcal_2);

	/* temp_std */
	snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s", battery_id, "temp_std");
	ret = of_property_read_u32_array(np, prop_name, &fuelgauge->info.temp_std, 1);
	if (ret < 0)
		PINFO("Can get prop %s (%d)\n", prop_name, ret);
	PINFO("%s = <%d>\n", prop_name, fuelgauge->info.temp_std);

	/* temp_volcal */
	snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s", battery_id, "temp_volcal");
	ret = of_property_read_u32_array(np, prop_name, temp_volcal, 3);
	if (ret < 0)
		PINFO("Can get prop %s (%d)\n", prop_name, ret);
	fuelgauge->info.en_fg_temp_volcal = temp_volcal[0];
	fuelgauge->info.fg_temp_volcal_denom = temp_volcal[1];
	fuelgauge->info.fg_temp_volcal_fact = temp_volcal[2];
	PINFO("%s = <%d, %d, %d>\n", prop_name,
		fuelgauge->info.en_fg_temp_volcal, fuelgauge->info.fg_temp_volcal_denom, fuelgauge->info.fg_temp_volcal_fact);

	/* temp_offset */
	snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s", battery_id, "temp_offset");
	ret = of_property_read_u32_array(np, prop_name, temp_offset, 6);
	if (ret < 0)
		PINFO("Can get prop %s (%d)\n", prop_name, ret);
	fuelgauge->info.en_high_fg_temp_offset = temp_offset[0];
	fuelgauge->info.high_fg_temp_offset_denom = temp_offset[1];
	fuelgauge->info.high_fg_temp_offset_fact = temp_offset[2];
	fuelgauge->info.en_low_fg_temp_offset = temp_offset[3];
	fuelgauge->info.low_fg_temp_offset_denom = temp_offset[4];
	fuelgauge->info.low_fg_temp_offset_fact = temp_offset[5];
	PINFO("%s = <%d, %d, %d, %d, %d, %d>\n", prop_name,
		fuelgauge->info.en_high_fg_temp_offset,
		fuelgauge->info.high_fg_temp_offset_denom, fuelgauge->info.high_fg_temp_offset_fact,
		fuelgauge->info.en_low_fg_temp_offset,
		fuelgauge->info.low_fg_temp_offset_denom, fuelgauge->info.low_fg_temp_offset_fact);

	/* temp_calc */
	snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s", battery_id, "temp_cal");
	ret = of_property_read_u32_array(np, prop_name, temp_cal, 10);
	if (ret < 0)
		PINFO("Can get prop %s (%d)\n", prop_name, ret);
	fuelgauge->info.en_high_fg_temp_cal = temp_cal[0];
	fuelgauge->info.high_fg_temp_p_cal_denom = temp_cal[1];
	fuelgauge->info.high_fg_temp_p_cal_fact = temp_cal[2];
	fuelgauge->info.high_fg_temp_n_cal_denom = temp_cal[3];
	fuelgauge->info.high_fg_temp_n_cal_fact = temp_cal[4];
	fuelgauge->info.en_low_fg_temp_cal = temp_cal[5];
	fuelgauge->info.low_fg_temp_p_cal_denom = temp_cal[6];
	fuelgauge->info.low_fg_temp_p_cal_fact = temp_cal[7];
	fuelgauge->info.low_fg_temp_n_cal_denom = temp_cal[8];
	fuelgauge->info.low_fg_temp_n_cal_fact = temp_cal[9];
	PINFO("%s = <%d, %d, %d, %d, %d, %d, %d, %d, %d, %d>\n", prop_name,
		fuelgauge->info.en_high_fg_temp_cal,
		fuelgauge->info.high_fg_temp_p_cal_denom, fuelgauge->info.high_fg_temp_p_cal_fact,
		fuelgauge->info.high_fg_temp_n_cal_denom, fuelgauge->info.high_fg_temp_n_cal_fact,
		fuelgauge->info.en_low_fg_temp_cal,
		fuelgauge->info.low_fg_temp_p_cal_denom, fuelgauge->info.low_fg_temp_p_cal_fact,
		fuelgauge->info.low_fg_temp_n_cal_denom, fuelgauge->info.low_fg_temp_n_cal_fact);

	/* ext_temp_calc */
	snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s", battery_id, "ext_temp_cal");
	ret = of_property_read_u32_array(np, prop_name, ext_temp_cal, 10);
	if (ret < 0)
		PINFO("Can get prop %s (%d)\n", prop_name, ret);
	fuelgauge->info.en_high_temp_cal = ext_temp_cal[0];
	fuelgauge->info.high_temp_p_cal_denom = ext_temp_cal[1];
	fuelgauge->info.high_temp_p_cal_fact = ext_temp_cal[2];
	fuelgauge->info.high_temp_n_cal_denom = ext_temp_cal[3];
	fuelgauge->info.high_temp_n_cal_fact = ext_temp_cal[4];
	fuelgauge->info.en_low_temp_cal = ext_temp_cal[5];
	fuelgauge->info.low_temp_p_cal_denom = ext_temp_cal[6];
	fuelgauge->info.low_temp_p_cal_fact = ext_temp_cal[7];
	fuelgauge->info.low_temp_n_cal_denom = ext_temp_cal[8];
	fuelgauge->info.low_temp_n_cal_fact = ext_temp_cal[9];
	PINFO("%s = <%d, %d, %d, %d, %d, %d, %d, %d, %d, %d>\n", prop_name,
		fuelgauge->info.en_high_temp_cal,
		fuelgauge->info.high_temp_p_cal_denom, fuelgauge->info.high_temp_p_cal_fact,
		fuelgauge->info.high_temp_n_cal_denom, fuelgauge->info.high_temp_n_cal_fact,
		fuelgauge->info.en_low_temp_cal,
		fuelgauge->info.low_temp_p_cal_denom, fuelgauge->info.low_temp_p_cal_fact,
		fuelgauge->info.low_temp_n_cal_denom, fuelgauge->info.low_temp_n_cal_fact);

	/* tem poff level */
	snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s", battery_id, "tem_poff");
	ret = of_property_read_u32_array(np, prop_name, set_temp_poff, 4);
	if (ret < 0)
		PINFO("Can get prop %s (%d)\n", prop_name, ret);
	fuelgauge->info.n_tem_poff = set_temp_poff[0];
	fuelgauge->info.n_tem_poff_offset = set_temp_poff[1];
	fuelgauge->info.l_tem_poff = set_temp_poff[2];
	fuelgauge->info.l_tem_poff_offset = set_temp_poff[3];

	PINFO("%s = <%d, %d, %d, %d>\n",
		prop_name,
		fuelgauge->info.n_tem_poff, fuelgauge->info.n_tem_poff_offset,
		fuelgauge->info.l_tem_poff, fuelgauge->info.l_tem_poff_offset);

	/* batt data version */
	snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s", battery_id, "data_ver");
	ret = of_property_read_u32_array(np, prop_name, &fuelgauge->info.data_ver, 1);
	if (ret < 0)
		PINFO("Can get prop %s (%d)\n", prop_name, ret);
	PINFO("%s = <%d>\n", prop_name, fuelgauge->info.data_ver);

	return 0;
}
#else
static int sm5708_fg_parse_dt(struct sec_fuelgauge_info *fuelgauge)
{
	return 0;
}
#endif

bool sm5708_fg_fuelalert_init(struct i2c_client *client, int soc)
{
	struct sec_fuelgauge_info *fuelgauge = i2c_get_clientdata(client);
	int ret;
	int value_v_alarm, value_soc_alarm;

	if (soc >= 0) {
		/* remove interrupt */
		ret = sm5708_fg_i2c_read_word(client, SM5708_REG_INTFG);

		/* check status */
		ret = sm5708_fg_i2c_read_word(client, SM5708_REG_STATUS);

		/* remove all mask */
		sm5708_fg_i2c_write_word(client, SM5708_REG_INTFG_MASK, 0x0000);

		/* enable volt alert only, other alert mask*/
		ret = MASK_L_SOC_INT|MASK_H_TEM_INT|MASK_L_TEM_INT;
		sm5708_fg_i2c_write_word(client, SM5708_REG_INTFG_MASK, ret);
		fuelgauge->info.irq_ctrl = ~(ret);

		/* set volt and soc alert threshold */
		value_v_alarm = (((fuelgauge->info.value_v_alarm)<<8)/1000);
		sm5708_fg_i2c_write_word(client, SM5708_REG_V_ALARM, value_v_alarm);
		value_soc_alarm = 0x0100; /* 1.00% */
		sm5708_fg_i2c_write_word(client, SM5708_REG_SOC_ALARM, value_soc_alarm);

		/* enabel volt alert control, other alert disable */
		ret = sm5708_fg_i2c_read_word(client, SM5708_REG_CNTL);
		ret = ret | ENABLE_V_ALARM;
		ret = ret & (~ENABLE_SOC_ALARM & ~ENABLE_T_H_ALARM & ~ENABLE_T_L_ALARM);
		sm5708_fg_i2c_write_word(client, SM5708_REG_CNTL, ret);

		pr_info("%s: irq_ctrl=0x%x, REG_CNTL=0x%x, V_ALARM=%d, SOC_ALARM=0x%x\n",
			__func__, fuelgauge->info.irq_ctrl, ret, value_v_alarm, value_soc_alarm);
	}

	/* alert flag init*/
	fuelgauge->info.soc_alert_flag = false;
	fuelgauge->is_fuel_alerted = false;

	return true;
}

bool sm5708_fg_is_fuelalerted(struct i2c_client *client)
{
	struct sec_fuelgauge_info *fuelgauge = i2c_get_clientdata(client);
	int ret;

	/* alert process */
	ret = sm5708_fg_i2c_read_word(client, SM5708_REG_INTFG);
	pr_info("%s: SM5708_REG_INTFG(0x%x)\n",
		__func__, ret);

	if (ret & fuelgauge->info.irq_ctrl) {
		/* check status */
		ret = sm5708_fg_i2c_read_word(client, SM5708_REG_STATUS);
		pr_info("%s: SM5708_REG_STATUS(0x%x)\n",
			__func__, ret);

		if (ret & fuelgauge->info.irq_ctrl) {
			return true;
		}
	}

	return false;
}

bool sm5708_fg_fuelalert_process(void *irq_data, bool is_fuel_alerted)
{
	struct sec_fuelgauge_info *fuelgauge = irq_data;
	struct i2c_client *client = fuelgauge->client;
	int ret;

	pr_info("%s: is_fuel_alerted=%d\n", __func__, is_fuel_alerted);

	if (is_fuel_alerted) {
		ret = sm5708_fg_i2c_read_word(client, SM5708_REG_STATUS);
		pr_info("%s: SM5708_REG_STATUS(0x%x)\n",
			__func__, ret);

		/* not use SOC alarm
		if (ret & fuelgauge->info.irq_ctrl & ENABLE_SOC_ALARM) {
			fuelgauge->info.soc_alert_flag = true;
			 // todo more action
		}
		*/

		if (ret & fuelgauge->info.irq_ctrl & ENABLE_V_ALARM) {
			fuelgauge->info.volt_alert_flag = true;
			/* todo more action */
		}
	}

	return true;
}

/* capacity is	0.1% unit */
static void sm5708_fg_get_scaled_capacity(
				struct sec_fuelgauge_info *fuelgauge,
				union power_supply_propval *val)
{
	val->intval = (val->intval < fuelgauge->pdata->capacity_min) ?
		0 : ((val->intval - fuelgauge->pdata->capacity_min) * 1000 /
		(fuelgauge->capacity_max - fuelgauge->pdata->capacity_min));

	pr_info("%s: scaled capacity (%d.%d)\n",
		__func__, val->intval/10, val->intval%10);
}

/* capacity is integer */
static void sm5708_fg_get_atomic_capacity(
				struct sec_fuelgauge_info *fuelgauge,
				union power_supply_propval *val)
{
	pr_info("%s : NOW(%d), OLD(%d)\n",
		__func__, val->intval, fuelgauge->capacity_old);

	if (fuelgauge->pdata->capacity_calculation_type &
		SEC_FUELGAUGE_CAPACITY_TYPE_ATOMIC) {
		if (fuelgauge->capacity_old < val->intval)
			val->intval = fuelgauge->capacity_old + 1;
		else if (fuelgauge->capacity_old > val->intval)
			val->intval = fuelgauge->capacity_old - 1;
	}

	/* keep SOC stable in abnormal status */
	if (fuelgauge->pdata->capacity_calculation_type &
		SEC_FUELGAUGE_CAPACITY_TYPE_SKIP_ABNORMAL) {
		if (!fuelgauge->ta_exist &&
			fuelgauge->capacity_old < val->intval) {
			pr_info("%s: capacity (old %d : new %d)\n",
				__func__, fuelgauge->capacity_old, val->intval);
			val->intval = fuelgauge->capacity_old;
		}
	}

	/* updated old capacity */
	fuelgauge->capacity_old = val->intval;
}

static int sm5708_fg_check_capacity_max(
				struct sec_fuelgauge_info *fuelgauge, int capacity_max)
{
	int cap_max, cap_min;

	cap_max = (fuelgauge->pdata->capacity_max +
				fuelgauge->pdata->capacity_max_margin) * 100 / 101;
	cap_min = (fuelgauge->pdata->capacity_max -
		fuelgauge->pdata->capacity_max_margin) * 100 / 101;

	return (capacity_max < cap_min) ? cap_min :
			((capacity_max > cap_max) ? cap_max : capacity_max);
}

static int sm5708_fg_calculate_dynamic_scale(
				struct sec_fuelgauge_info *fuelgauge, int capacity)
{
	union power_supply_propval raw_soc_val;

	raw_soc_val.intval = sm5708_get_soc(fuelgauge->client);

	if (raw_soc_val.intval <
		fuelgauge->pdata->capacity_max -
		fuelgauge->pdata->capacity_max_margin) {
		pr_info("%s: raw soc(%d) is very low, skip routine\n",
				__func__, raw_soc_val.intval);
		return fuelgauge->capacity_max;
	} else {
		fuelgauge->capacity_max =
			(raw_soc_val.intval >
			fuelgauge->pdata->capacity_max +
			fuelgauge->pdata->capacity_max_margin) ?
			(fuelgauge->pdata->capacity_max +
			fuelgauge->pdata->capacity_max_margin) :
			raw_soc_val.intval;
			pr_debug("%s: raw soc (%d)", __func__,
					fuelgauge->capacity_max);
	}

	fuelgauge->capacity_max =
		(fuelgauge->capacity_max * 100 / (capacity + 1));
	fuelgauge->capacity_old = capacity;

	pr_info("%s: %d is used for capacity_max, capacity(%d)\n",
		__func__, fuelgauge->capacity_max, capacity);

	return fuelgauge->capacity_max;
}

bool sm5708_fg_reset(struct i2c_client *client)
{
	pr_info("%s: Start quick-start\n", __func__);
	/* SW reset code */
	sm5708_fg_i2c_verified_write_word(client, SM5708_REG_RESET, SW_RESET_CODE);
	/* delay 800ms */
	msleep(800);
	/* init code */
	sm5708_fg_init(client, false);

	pr_info("%s: End quick-start\n", __func__);
	return true;
}

static void sm5708_fg_reset_capacity_by_jig_connection(struct sec_fuelgauge_info *fuelgauge)
{
	union power_supply_propval value;
	int ret;

	pr_info("%s: (Jig Connection)\n", __func__);

	ret = sm5708_fg_i2c_read_word(fuelgauge->client, SM5708_REG_RESERVED);
	ret |= SM5708_JIG_CONNECTED;
	sm5708_fg_i2c_write_word(fuelgauge->client, SM5708_REG_RESERVED, ret);
	/* If JIG is attached, the voltage is set as 1079 */
	value.intval = 1079;
	psy_do_property("battery", set,
			POWER_SUPPLY_PROP_VOLTAGE_NOW, value);
}

static int sm5708_fg_get_property(struct power_supply *psy,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct sec_fuelgauge_info *fuelgauge =
		container_of(psy, struct sec_fuelgauge_info, psy_fg);

	switch (psp) {
	/* Additional Voltage Information (mV) */
	case POWER_SUPPLY_PROP_PRESENT:
		/* SM5708 is not suport this prop */
		sm5708_fg_get_batt_present(fuelgauge->client);
		break;
	/* Cell voltage (VCELL, mV) */
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		sm5708_get_vbat(fuelgauge->client);
		val->intval = fuelgauge->info.batt_voltage;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_AVG:
		switch (val->intval) {
		case SEC_BATTERY_VOLTAGE_AVERAGE:
			sm5708_get_vbat(fuelgauge->client);
			val->intval = fuelgauge->info.batt_avgvoltage;
			break;
		case SEC_BATTERY_VOLTAGE_OCV:
			sm5708_get_ocv(fuelgauge->client);
			val->intval = fuelgauge->info.batt_ocv;
			break;
		}
		break;
	/* Current (mA) */
	case POWER_SUPPLY_PROP_INPUT_VOLTAGE_REGULATION:
		val->intval = sm5708_fg_get_jig_mode_real_vbat(fuelgauge->client);
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		sm5708_get_curr(fuelgauge->client);
		if (val->intval == SEC_BATTERY_CURRENT_UA)
			val->intval = fuelgauge->info.batt_current * 1000;
		else
			val->intval = fuelgauge->info.batt_current;
		break;
	/* Average Current (mA) */
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		sm5708_get_curr(fuelgauge->client);
		if (val->intval == SEC_BATTERY_CURRENT_UA)
			val->intval = fuelgauge->info.batt_avgcurrent * 1000;
		else
			val->intval = fuelgauge->info.batt_avgcurrent;
		break;
	/* Battery Temperature */
	case POWER_SUPPLY_PROP_TEMP:
	/* Target Temperature */
	case POWER_SUPPLY_PROP_TEMP_AMBIENT:
		sm5708_get_temperature(fuelgauge->client);
		val->intval = fuelgauge->info.temp_fg;
		break;
	/* SOC (%) */
	case POWER_SUPPLY_PROP_CAPACITY:
		sm5708_get_all_value(fuelgauge->client);
		/* SM5708 F/G unit is 0.1%, raw ==> convert the unit to 0.01% */
		if (val->intval == SEC_FUELGAUGE_CAPACITY_TYPE_RAW) {
			val->intval = fuelgauge->info.batt_soc * 10;
			break;
		} else
			val->intval = fuelgauge->info.batt_soc;

		if (fuelgauge->pdata->capacity_calculation_type &
			(SEC_FUELGAUGE_CAPACITY_TYPE_SCALE |
			 SEC_FUELGAUGE_CAPACITY_TYPE_DYNAMIC_SCALE))
			sm5708_fg_get_scaled_capacity(fuelgauge, val);

		/* capacity should be between 0% and 100%
		 * (0.1% degree)
		 */
		if (val->intval > 1000)
			val->intval = 1000;
		if (val->intval < 0)
			val->intval = 0;

		/* get only integer part */
		val->intval /= 10;

		/* check whether doing the wake_unlock */
		if ((val->intval > fuelgauge->pdata->fuel_alert_soc) &&
			fuelgauge->is_fuel_alerted) {
			wake_unlock(&fuelgauge->fuel_alert_wake_lock);
			sm5708_fg_fuelalert_init(fuelgauge->client,
					fuelgauge->pdata->fuel_alert_soc);
		}

		/* (Only for atomic capacity)
		 * In initial time, capacity_old is 0.
		 * and in resume from sleep,
		 * capacity_old is too different from actual soc.
		 * should update capacity_old
		 * by val->intval in booting or resume.
		 */
		if (fuelgauge->initial_update_of_soc) {
			/* updated old capacity */
			fuelgauge->capacity_old = val->intval;
			fuelgauge->initial_update_of_soc = false;
			break;
		}

		if (fuelgauge->pdata->capacity_calculation_type &
			(SEC_FUELGAUGE_CAPACITY_TYPE_ATOMIC |
			 SEC_FUELGAUGE_CAPACITY_TYPE_SKIP_ABNORMAL))
			sm5708_fg_get_atomic_capacity(fuelgauge, val);
		break;
	case POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN:
		val->intval = fuelgauge->capacity_max;
		break;
	case POWER_SUPPLY_PROP_STATUS:
	case POWER_SUPPLY_PROP_CHARGE_FULL:
	case POWER_SUPPLY_PROP_ENERGY_NOW:
		return -ENODATA;
	default:
		return -EINVAL;
	}
	return 0;
}

static int sm5708_fg_set_property(struct power_supply *psy,
				enum power_supply_property psp,
				const union power_supply_propval *val)
{
	struct sec_fuelgauge_info *fuelgauge =
		container_of(psy, struct sec_fuelgauge_info, psy_fg);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		if (val->intval == POWER_SUPPLY_STATUS_FULL) {
			fuelgauge->info.flag_full_charge = 1;
#if defined(CONFIG_BATTERY_AGE_FORECAST)
			pr_info("%s: POWER_SUPPLY_STATUS_FULL : q_max_now = 0x%x \n", __func__, fuelgauge->info.q_max_now);
			if (fuelgauge->info.q_max_now !=
				fuelgauge->info.q_max_table[get_v_max_index_by_cycle(fuelgauge->client)]) {
				if (!sm5708_fg_reset(fuelgauge->client))
					return -EINVAL;
			}
#endif
		}
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		if (fuelgauge->pdata->capacity_calculation_type &
				SEC_FUELGAUGE_CAPACITY_TYPE_DYNAMIC_SCALE)
				sm5708_fg_calculate_dynamic_scale(fuelgauge, val->intval);
#if defined(CONFIG_BATTERY_AGE_FORECAST)
		pr_info("%s: POWER_SUPPLY_PROP_CHARGE_FULL : q_max_now = 0x%x \n", __func__, fuelgauge->info.q_max_now);
		if (fuelgauge->info.q_max_now !=
			fuelgauge->info.q_max_table[get_v_max_index_by_cycle(fuelgauge->client)]) {
			if (!sm5708_fg_reset(fuelgauge->client))
				return -EINVAL;
		}
#endif
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		fuelgauge->cable_type = val->intval;
		if (val->intval == POWER_SUPPLY_TYPE_BATTERY) {
			fuelgauge->ta_exist = false;
			fuelgauge->is_charging = false;
		} else {
			fuelgauge->ta_exist = true;
			fuelgauge->is_charging = true;
		}
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		if (val->intval == SEC_FUELGAUGE_CAPACITY_TYPE_RESET) {
			fuelgauge->initial_update_of_soc = true;
			if (!sm5708_fg_reset(fuelgauge->client))
				return -EINVAL;
			else
				break;
		}
	case POWER_SUPPLY_PROP_TEMP:
		fuelgauge->info.temperature = val->intval;
		break;
	case POWER_SUPPLY_PROP_TEMP_AMBIENT:
		break;
	case POWER_SUPPLY_PROP_INPUT_VOLTAGE_REGULATION:
		break;
	case POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN:
		pr_info("%s: capacity_max changed, %d -> %d\n",
				__func__, fuelgauge->capacity_max, val->intval);
		fuelgauge->capacity_max = sm5708_fg_check_capacity_max(fuelgauge, val->intval);
		fuelgauge->initial_update_of_soc = true;
		break;
	case POWER_SUPPLY_PROP_ENERGY_NOW:
		sm5708_fg_reset_capacity_by_jig_connection(fuelgauge);
		break;
#if defined(CONFIG_BATTERY_AGE_FORECAST)
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		pr_info("%s: full condition soc changed, %d -> %d\n",
			__func__, fuelgauge->chg_full_soc, val->intval);
		fuelgauge->chg_full_soc = val->intval;
		break;
#endif
	default:
		return -EINVAL;
	}
	return 0;
}

static void sm5708_fg_isr_work(struct work_struct *work)
{
	struct sec_fuelgauge_info *fuelgauge =
		container_of(work, struct sec_fuelgauge_info, isr_work.work);

	/* process for fuel gauge chip */
	sm5708_fg_fuelalert_process(fuelgauge, fuelgauge->is_fuel_alerted);

	/* process for others */
	if (fuelgauge->pdata->fuelalert_process != NULL)
		fuelgauge->pdata->fuelalert_process(fuelgauge->is_fuel_alerted);
}

static irqreturn_t sm5708_fg_irq_thread(int irq, void *irq_data)
{
	struct sec_fuelgauge_info *fuelgauge = irq_data;
	bool fuel_alerted;

	if (fuelgauge->pdata->fuel_alert_soc >= 0) {
		fuel_alerted =
			sm5708_fg_is_fuelalerted(fuelgauge->client);

		pr_info("%s: Fuel-alert %salerted!\n",
			__func__, fuel_alerted ? "" : "NOT ");

		if (fuel_alerted == fuelgauge->is_fuel_alerted) {
			if (!fuelgauge->pdata->repeated_fuelalert) {
				dev_dbg(&fuelgauge->client->dev,
					"%s: Fuel-alert Repeated (%d)\n",
					__func__, fuelgauge->is_fuel_alerted);
				return IRQ_HANDLED;
			}
		}

		if (fuel_alerted)
			wake_lock(&fuelgauge->fuel_alert_wake_lock);
		else
			wake_unlock(&fuelgauge->fuel_alert_wake_lock);

		fuelgauge->is_fuel_alerted = fuel_alerted;

		schedule_delayed_work(&fuelgauge->isr_work, 0);
	}

	return IRQ_HANDLED;
}

static int sm5708_create_attrs(struct device *dev)
{
	int i, rc;

	for (i = 0; i < ARRAY_SIZE(sm5708_fg_attrs); i++) {
		rc = device_create_file(dev, &sm5708_fg_attrs[i]);
		if (rc)
			goto create_attrs_failed;
	}
	goto create_attrs_succeed;

create_attrs_failed:
	dev_err(dev, "%s: failed (%d)\n", __func__, rc);
	while (i--)
		device_remove_file(dev, &sm5708_fg_attrs[i]);
create_attrs_succeed:
	return rc;
}

ssize_t sm5708_fg_show_attrs(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	const ptrdiff_t offset = attr - sm5708_fg_attrs;
	//struct sec_fuelgauge_info *fuelgauge = dev_get_drvdata(dev);
	struct power_supply *psy = dev_get_drvdata(dev);
	struct sec_fuelgauge_info *fuelgauge =
		container_of(psy, struct sec_fuelgauge_info, psy_fg);
	int i = 0, j = 0;
    u8 reg = 0;
    int reg_data = 0;

	switch (offset) {
	case FG_REG:
	case FG_REGS:		
		break;
	case FG_DATA:
		/* 0x00 ~ 0x5f, 0x80~0xbf */
	    for (j = 0; j < 12; j++) {
			for (reg = 0; reg < 0x10; reg++) {
	            reg_data = sm5708_fg_i2c_read_word(fuelgauge->client, reg + j * 0x10);
				i += scnprintf(buf + i, PAGE_SIZE - i, "0x%02x:\t0x%04x\n", reg + j * 0x10, reg_data);
			}
	        if (j == 5)
	            j = 7;
	    }
		break;
	default:
		i = -EINVAL;
		break;
	}

	return i;
}

ssize_t sm5708_fg_store_attrs(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	const ptrdiff_t offset = attr - sm5708_fg_attrs;
	//struct sec_fuelgauge_info *fuelgauge = dev_get_drvdata(dev);
	struct power_supply *psy = dev_get_drvdata(dev);
	struct sec_fuelgauge_info *fuelgauge =
		container_of(psy, struct sec_fuelgauge_info, psy_fg);

	int ret = 0;
	int x = 0, y = 0;

	switch (offset) {
	case FG_REG:
	case FG_REGS:
		break;
	case FG_DATA:
		if (sscanf(buf, "0x%8x 0x%8x", &x, &y) == 2) {
			if (x >= 0x00 && x <= 0xff) {
				u8 addr = x;
				u16 data = y;
				pr_info("%s FG_DATA write : 0x%x = 0x%x \n", __func__, addr, data);
				if (sm5708_fg_i2c_write_word(fuelgauge->client, addr, data)) {
					pr_err("%s: addr: 0x%x write fail\n", __func__, addr);
				}
			} else {
				pr_err("%s: addr: 0x%x is wrong\n", __func__, x);
			}
		}
		ret = count;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

static int sm5708_fuelgauge_probe(struct i2c_client *client,
						const struct i2c_device_id *id)
{
	struct i2c_adapter *adapter = to_i2c_adapter(client->dev.parent);
	struct sec_fuelgauge_info *fuelgauge;
	sec_battery_platform_data_t *pdata = NULL;
/*	struct battery_data_t *battery_data = NULL; */
	int ret = 0;
	union power_supply_propval raw_soc_val;

	pr_info("%s: SM5708 Fuelgauge Driver Loading\n", __func__);

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE))
		return -EIO;

	fuelgauge = kzalloc(sizeof(*fuelgauge), GFP_KERNEL);
	if (!fuelgauge)
		return -ENOMEM;

	mutex_init(&fuelgauge->fg_lock);

	fuelgauge->client = client;

	if (client->dev.of_node) {
		int error;
		pdata = devm_kzalloc(&client->dev, sizeof(sec_battery_platform_data_t), GFP_KERNEL);
		if (!pdata) {
			dev_err(&client->dev, "Failed to allocate memory\n");
			ret = -ENOMEM;
			goto err_free;
		}

		fuelgauge->pdata = pdata;

		mutex_init(&fuelgauge->info.param_lock);
		mutex_lock(&fuelgauge->info.param_lock);
#if defined(CONFIG_BATTERY_AGE_FORECAST)
		temp_parse_dt(fuelgauge);
#endif
		error = sm5708_fg_parse_dt(fuelgauge);
		mutex_unlock(&fuelgauge->info.param_lock);
		if (error < 0) {
			dev_err(&client->dev,
				"%s: Failed to get fuel_int\n", __func__);
			goto err_parse_dt;
		}
	} else	{
		dev_err(&client->dev,
			"%s: Failed to get of_node\n", __func__);
		fuelgauge->pdata = client->dev.platform_data;
	}
	i2c_set_clientdata(client, fuelgauge);

	if (fuelgauge->pdata->fg_gpio_init != NULL) {
		dev_err(&client->dev,
				"%s: @@@\n", __func__);
		if (!fuelgauge->pdata->fg_gpio_init()) {
			dev_err(&client->dev,
					"%s: Failed to Initialize GPIO\n", __func__);
			goto err_devm_free;
		}
	}

	if (!sm5708_fg_init(fuelgauge->client, false)) {
		dev_err(&client->dev,
			"%s: Failed to Initialize Fuelgauge\n", __func__);
		goto err_devm_free;
	}

	fuelgauge->psy_fg.name		= "sm5708-fuelgauge";
	fuelgauge->psy_fg.type		= POWER_SUPPLY_TYPE_UNKNOWN;
	fuelgauge->psy_fg.get_property	= sm5708_fg_get_property;
	fuelgauge->psy_fg.set_property	= sm5708_fg_set_property;
	fuelgauge->psy_fg.properties	= sm5708_fuelgauge_props;
	fuelgauge->psy_fg.num_properties =
		ARRAY_SIZE(sm5708_fuelgauge_props);

		fuelgauge->capacity_max = fuelgauge->pdata->capacity_max;
	raw_soc_val.intval = sm5708_get_soc(fuelgauge->client);

		if (raw_soc_val.intval > fuelgauge->pdata->capacity_max)
				sm5708_fg_calculate_dynamic_scale(fuelgauge, 100);

	ret = (__u64) power_supply_register(&client->dev, fuelgauge->psy_fg.desc, fuelgauge->psy_fg.config);
	if (ret) {
		dev_err(&client->dev,
			"%s: Failed to Register psy_fg\n", __func__);
		goto err_free;
	}

	fuelgauge->is_fuel_alerted = false;
	if (fuelgauge->pdata->fuel_alert_soc >= 0) {
		if (sm5708_fg_fuelalert_init(fuelgauge->client,
			fuelgauge->pdata->fuel_alert_soc))
			wake_lock_init(&fuelgauge->fuel_alert_wake_lock,
				WAKE_LOCK_SUSPEND, "fuel_alerted");
		else {
			dev_err(&client->dev,
				"%s: Failed to Initialize Fuel-alert\n",
				__func__);
			goto err_irq;
		}
	}

	if (fuelgauge->pdata->fg_irq > 0) {
		INIT_DELAYED_WORK(
			&fuelgauge->isr_work, sm5708_fg_isr_work);

		fuelgauge->fg_irq = gpio_to_irq(fuelgauge->pdata->fg_irq);
		pr_info(
			"%s: fg_irq = %d\n", __func__, fuelgauge->fg_irq);
		if (fuelgauge->fg_irq > 0) {
			ret = request_threaded_irq(fuelgauge->fg_irq,
					NULL, sm5708_fg_irq_thread,
					IRQF_TRIGGER_FALLING
					 | IRQF_ONESHOT,
					"fuelgauge-irq", fuelgauge);
			if (ret) {
				dev_err(&client->dev,
					"%s: Failed to Reqeust IRQ\n", __func__);
				goto err_supply_unreg;
			}

			ret = enable_irq_wake(fuelgauge->fg_irq);
			if (ret < 0)
				dev_err(&client->dev,
					"%s: Failed to Enable Wakeup Source(%d)\n",
					__func__, ret);
		} else {
			dev_err(&client->dev, "%s: Failed gpio_to_irq(%d)\n",
				__func__, fuelgauge->fg_irq);
			goto err_supply_unreg;
		}
	}

	fuelgauge->initial_update_of_soc = true;
	fuelgauge->info.temperature = 250;

	/* if (sec_bat_check_jig_status())
		sm5708_fg_reset_capacity_by_jig_connection(fuelgauge); */

	ret = sm5708_create_attrs(&fuelgauge->psy_fg.dev);
	if (ret) {
		dev_err(&client->dev,
			"%s : Failed to create_attrs\n", __func__);
		goto err_irq;
	}

	pr_info(
		"%s: SEC Fuelgauge Driver Loaded\n", __func__);
	return 0;

err_irq:
	if (fuelgauge->fg_irq > 0)
		free_irq(fuelgauge->fg_irq, fuelgauge);
	wake_lock_destroy(&fuelgauge->fuel_alert_wake_lock);
err_supply_unreg:
	power_supply_unregister(&fuelgauge->psy_fg);
err_devm_free:
err_parse_dt:
	if (pdata)
		devm_kfree(&client->dev, pdata);
/*	if (battery_data)
		devm_kfree(&client->dev, battery_data); */
err_free:
	mutex_destroy(&fuelgauge->fg_lock);
	kfree(fuelgauge);

	pr_info("%s: Fuel gauge probe failed\n", __func__);
	return ret;
}

static int sm5708_fuelgauge_remove(
						struct i2c_client *client)
{
	struct sec_fuelgauge_info *fuelgauge = i2c_get_clientdata(client);

	if (fuelgauge->pdata->fuel_alert_soc >= 0)
		wake_lock_destroy(&fuelgauge->fuel_alert_wake_lock);

	return 0;
}

static int sm5708_fuelgauge_suspend(struct device *dev)
{
	return 0;
}

static int sm5708_fuelgauge_resume(struct device *dev)
{
	return 0;
}

static void sm5708_fuelgauge_shutdown(struct i2c_client *client)
{
}

static const struct i2c_device_id sm5708_fuelgauge_id[] = {
	{"sm5708-fuelgauge", 0},
	{}
};

static const struct dev_pm_ops sm5708_fuelgauge_pm_ops = {
	.suspend = sm5708_fuelgauge_suspend,
	.resume	 = sm5708_fuelgauge_resume,
};

MODULE_DEVICE_TABLE(i2c, sm5708_fuelgauge_id);
static struct of_device_id fuelgague_i2c_match_table[] = {
	{ .compatible = "sm5708-fuelgauge,i2c", },
	{ },
};
MODULE_DEVICE_TABLE(i2c, fuelgague_i2c_match_table);

static struct i2c_driver sm5708_fuelgauge_driver = {
	.driver = {
		   .name = "sm5708-fuelgauge",
		   .owner = THIS_MODULE,
		   .of_match_table = fuelgague_i2c_match_table,
#ifdef CONFIG_PM
		   .pm = &sm5708_fuelgauge_pm_ops,
#endif
	},
	.probe	= sm5708_fuelgauge_probe,
	.remove	= sm5708_fuelgauge_remove,
	.shutdown	= sm5708_fuelgauge_shutdown,
	.id_table	= sm5708_fuelgauge_id,
};

static int __init sm5708_fuelgauge_init(void)
{
	pr_info("%s\n", __func__);

	return i2c_add_driver(&sm5708_fuelgauge_driver);
}

static void __exit sm5708_fuelgauge_exit(void)
{
	i2c_del_driver(&sm5708_fuelgauge_driver);
}

module_init(sm5708_fuelgauge_init);
module_exit(sm5708_fuelgauge_exit);

MODULE_DESCRIPTION("Samsung SM5708 Fuel Gauge Driver");
MODULE_AUTHOR("Samsung Electronics");
MODULE_LICENSE("GPL");
