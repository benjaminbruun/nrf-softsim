/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Onomondo ApS
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Battery state-of-charge for the Memfault battery metrics component,
 * read from the Thingy:91 X nPM1300 PMIC via the nRF Fuel Gauge library.
 *
 * On NCS >= 3.4 the application owns nrf_fuel_gauge_init()/process();
 * Memfault's nPM13xx port (memfault_platform_npm13xx_battery.c) only reads
 * SoC/SoH from the library and detects our init through --wrap.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/npm13xx_charger.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/util.h>

#include <nrf_fuel_gauge.h>
#include "lp803448_model.h"

LOG_MODULE_REGISTER(fuel_gauge);

/* ponytail: fixed 60 s cadence (the lib accepts irregular sampling); ceiling:
 * SOH estimation wants 2-5 Hz while charging, so battery_soh_pct hugs its
 * initial value. Upgrade path: shorten the period while the charger is active.
 */
#define SAMPLE_PERIOD K_SECONDS(60)

/* nPM13xx CHARGER.BCHGCHARGESTATUS register bitmasks */
#define NPM13XX_CHG_STATUS_COMPLETE_MASK BIT(1)
#define NPM13XX_CHG_STATUS_TRICKLE_MASK	 BIT(2)
#define NPM13XX_CHG_STATUS_CC_MASK	 BIT(3)
#define NPM13XX_CHG_STATUS_CV_MASK	 BIT(4)

static const struct device *charger = DEVICE_DT_GET(DT_NODELABEL(npm1300_charger));
static int64_t ref_time;

static int read_sensors(float *voltage, float *current, float *temp, int32_t *chg_status)
{
	struct sensor_value value;
	int ret;

	ret = sensor_sample_fetch(charger);
	if (ret < 0) {
		return ret;
	}

	sensor_channel_get(charger, SENSOR_CHAN_GAUGE_VOLTAGE, &value);
	*voltage = (float)value.val1 + ((float)value.val2 / 1000000);

	sensor_channel_get(charger, SENSOR_CHAN_GAUGE_TEMP, &value);
	*temp = (float)value.val1 + ((float)value.val2 / 1000000);

	sensor_channel_get(charger, SENSOR_CHAN_GAUGE_AVG_CURRENT, &value);
	*current = (float)value.val1 + ((float)value.val2 / 1000000);

	sensor_channel_get(charger, SENSOR_CHAN_NPM13XX_CHARGER_STATUS, &value);
	*chg_status = value.val1;

	return 0;
}

/* The secondary-cell library only lets SoC rise while it knows the battery is
 * charging, so charge-state updates are required for correctness.
 * ponytail: no VBUS/MFD callback - charge activity is learned from the status
 * register within one sample period; ceiling: TTF accuracy (unused here) and
 * CC vs CC_LIMITED disambiguation, so always report CC_LIMITED like the NCS
 * npm13xx_fuel_gauge sample does.
 */
static int charge_status_inform(int32_t chg_status)
{
	union nrf_fuel_gauge_ext_state_info_data state_info;

	if (chg_status & NPM13XX_CHG_STATUS_COMPLETE_MASK) {
		state_info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_COMPLETE;
	} else if (chg_status & NPM13XX_CHG_STATUS_TRICKLE_MASK) {
		state_info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_TRICKLE;
	} else if (chg_status & NPM13XX_CHG_STATUS_CC_MASK) {
		state_info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_CC_LIMITED;
	} else if (chg_status & NPM13XX_CHG_STATUS_CV_MASK) {
		state_info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_CV;
	} else {
		state_info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_IDLE;
	}

	return nrf_fuel_gauge_ext_state_update(NRF_FUEL_GAUGE_EXT_STATE_INFO_CHARGE_STATE_CHANGE,
					       &state_info);
}

static void sample_work_handler(struct k_work *work)
{
	static int32_t chg_status_prev = -1;
	float voltage;
	float current;
	float temp;
	float soc;
	int32_t chg_status;
	int err;

	if (read_sensors(&voltage, &current, &temp, &chg_status) == 0) {
		if (chg_status != chg_status_prev) {
			chg_status_prev = chg_status;
			charge_status_inform(chg_status);
		}

		/* Zephyr sensor API convention for gauge current is
		 * negative=discharging, the lib expects negative=charging.
		 * ref_time only advances here, so a skipped iteration's time
		 * is included in the next successful t_delta.
		 */
		err = nrf_fuel_gauge_process(voltage, -current, temp,
					     (float)k_uptime_delta(&ref_time) / 1000.f, &soc, NULL);
		__ASSERT(err == 0 && soc >= 0.0f && soc <= 100.0f,
			 "fuel gauge broken: err=%d soc=%d", err, (int)soc);
	}

	k_work_schedule(k_work_delayable_from_work(work), SAMPLE_PERIOD);
}

static K_WORK_DELAYABLE_DEFINE(sample_work, sample_work_handler);

static int fuel_gauge_setup(void)
{
	struct nrf_fuel_gauge_init_parameters parameters =
		NRF_FUEL_GAUGE_DEFAULT_INIT_PARAMETERS_SECONDARY(0.0f, 0.0f, 0.0f, &battery_model);
	struct sensor_value value;
	int32_t chg_status;
	int err;

	if (!device_is_ready(charger)) {
		LOG_ERR("charger device not ready");
		return -ENODEV;
	}

	err = read_sensors(&parameters.v0, &parameters.i0, &parameters.t0, &chg_status);
	if (err < 0) {
		LOG_ERR("initial charger read failed (%d)", err);
		return err;
	}
	parameters.i0 = -parameters.i0;

	/* ponytail: no state persistence - SOH/cycle count reset each boot;
	 * upgrade path: nrf_fuel_gauge_state_get() + settings_save_one() as in
	 * the NCS npm13xx_fuel_gauge sample.
	 */
	err = nrf_fuel_gauge_init(&parameters, NULL);
	if (err < 0) {
		LOG_ERR("fuel gauge init failed (%d)", err);
		return err;
	}

	/* Charge current limit and termination current (limit/10) feed the
	 * charge/SOH modelling behind battery_soh_pct.
	 */
	sensor_channel_get(charger, SENSOR_CHAN_GAUGE_DESIRED_CHARGING_CURRENT, &value);
	float max_charge_current = (float)value.val1 + ((float)value.val2 / 1000000);

	nrf_fuel_gauge_ext_state_update(
		NRF_FUEL_GAUGE_EXT_STATE_INFO_CHARGE_CURRENT_LIMIT,
		&(union nrf_fuel_gauge_ext_state_info_data){
			.charge_current_limit = max_charge_current});
	nrf_fuel_gauge_ext_state_update(
		NRF_FUEL_GAUGE_EXT_STATE_INFO_TERM_CURRENT,
		&(union nrf_fuel_gauge_ext_state_info_data){
			.charge_term_current = max_charge_current / 10.f});
	charge_status_inform(chg_status);

	ref_time = k_uptime_get();
	k_work_schedule(&sample_work, SAMPLE_PERIOD);

	LOG_INF("nRF Fuel Gauge %s, model %s", nrf_fuel_gauge_version, battery_model.name);

	return 0;
}

/* One priority before Memfault's own SYS_INIT (default 40) so the boot-time
 * SoC read that seeds the discharge session succeeds. All nPM1300 devices
 * init at POST_KERNEL, i.e. before any APPLICATION-level init.
 */
SYS_INIT(fuel_gauge_setup, APPLICATION, CONFIG_MEMFAULT_NRF_PLATFORM_BATTERY_NPM13XX_INIT_PRIORITY);
