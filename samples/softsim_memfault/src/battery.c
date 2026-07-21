/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Onomondo ApS
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Battery state-of-charge for the Memfault battery metrics component,
 * read from the Thingy:91 ADP536x PMIC fuel gauge (enabled by the
 * board init in nrf/boards/nordic/thingy91/adp5360_init.c).
 */

#include <adp536x.h>
#include <memfault/metrics/platform/battery.h>
#include <zephyr/sys/util.h>

/* STATUS1 register, CHARGER_STATUS field: 0 means charger off. */
#define ADP536X_CHARGER_STATUS_MASK 0x07
#define ADP536X_CHARGER_OFF 0

int memfault_platform_get_stateofcharge(sMfltPlatformBatterySoc *soc)
{
	uint8_t pct;
	uint8_t status;

	if (adp536x_charger_status_1_read(&status)) {
		/* Discharging state must always be valid; without it, report failure
		 * and let Memfault skip this sample entirely.
		 */
		return -1;
	}

	soc->discharging = (status & ADP536X_CHARGER_STATUS_MASK) == ADP536X_CHARGER_OFF;

	if (adp536x_fg_soc(&pct)) {
		return -1;
	}

	/* Memfault expects the SoC pre-scaled (Zephyr port default scale: 1000). */
	soc->soc = MIN(pct, 100) * CONFIG_MEMFAULT_METRICS_BATTERY_SOC_PCT_SCALE_VALUE;

	return 0;
}
