/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-2026 Onomondo ApS
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <stdio.h>
#include <unistd.h>

#include <nrf_softsim.h>
#include <modem/lte_lc.h>
#include <modem/nrf_modem_lib.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/reboot.h>

#include <memfault/core/data_packetizer.h>
#include <memfault/metrics/metrics.h>
#include <memfault/panics/coredump.h>
#include <memfault/ports/zephyr/http.h>

LOG_MODULE_REGISTER(softsim_sample, LOG_LEVEL_INF);

/* Headroom over the full SoftSIM profile (~410 chars incl. SMSP/PIN/SMSC) */
#define PROFILE_MAX_SIZE 512

/* Semaphores */
K_SEM_DEFINE(lte_connected, 0, 1);    /* Semaphore to signal LTE connection established */
K_SEM_DEFINE(profile_received, 0, 1); /* Semaphore to signal profile received */

struct rx_buf_t {
	char *buf;
	size_t len;
	size_t pos;
};

static const struct device *const uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart0));

static void lte_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if ((evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_HOME) &&
		    (evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_ROAMING)) {
			break;
		}

		LOG_INF("Network registration status: %s",
			evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME
				? "Connected - home network"
				: "Connected - roaming");
		k_sem_give(&lte_connected);
		break;
	case LTE_LC_EVT_PSM_UPDATE:
		LOG_INF("PSM parameter update: TAU: %d, Active time: %d", evt->psm_cfg.tau,
			evt->psm_cfg.active_time);
		break;
	case LTE_LC_EVT_EDRX_UPDATE: {
		char log_buf[60];
		ssize_t len;

		len = snprintf(log_buf, sizeof(log_buf), "eDRX parameter update: eDRX: %f, PTW: %f",
			       (double)evt->edrx_cfg.edrx, (double)evt->edrx_cfg.ptw);
		if (len > 0) {
			LOG_INF("%s", log_buf);
		}
		break;
	}
	case LTE_LC_EVT_RRC_UPDATE:
		LOG_INF("RRC mode: %s",
			evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED ? "Connected" : "Idle");
		break;
	case LTE_LC_EVT_CELL_UPDATE:
		LOG_INF("LTE cell changed: Cell ID: %d, Tracking area: %d", evt->cell.id,
			evt->cell.tac);
		break;
	default:
		break;
	}
}

static void modem_connect(void)
{
	int err = lte_lc_connect_async(lte_handler);
	if (err) {
		LOG_ERR("Connecting to LTE network failed, error: %d", err);
		return;
	}
}

/* Push whatever Memfault data is buffered (trace events, metrics, logs) to the
 * cloud. Coredump upload after a crash is handled internally by the NCS
 * integration (CONFIG_MEMFAULT_NCS_POST_COREDUMP_ON_NETWORK_CONNECTED);
 * everything else is also flushed periodically by
 * CONFIG_MEMFAULT_HTTP_PERIODIC_UPLOAD, so this only shortens the wait after a
 * (re)connect.
 */
static void memfault_post_on_connect(void)
{
#ifdef CONFIG_MEMFAULT_NCS_LTE_METRICS
	uint32_t time_to_lte_connection;

	memfault_metrics_heartbeat_timer_read(MEMFAULT_METRICS_KEY(ncs_lte_time_to_connect_ms),
					      &time_to_lte_connection);
	LOG_INF("Time to connect: %d ms", time_to_lte_connection);
#endif /* CONFIG_MEMFAULT_NCS_LTE_METRICS */

	if (IS_ENABLED(CONFIG_MEMFAULT_NCS_POST_COREDUMP_ON_NETWORK_CONNECTED) &&
	    memfault_coredump_has_valid_coredump(NULL)) {
		/* Coredump sending handled internally */
		return;
	}

	/* Close out the current heartbeat so metrics collected so far are sent. */
	memfault_metrics_heartbeat_debug_trigger();

	if (!memfault_packetizer_data_available()) {
		LOG_DBG("No Memfault data to send");
		return;
	}

	LOG_INF("Sending buffered data to Memfault");
	memfault_zephyr_port_post_data();
}

static void drain_logs_and_reboot(void)
{
	/* Flush the deferred log buffer over UART before the reboot discards it. */
	while (log_data_pending()) {
		log_process();
		k_yield();
	}
	sys_reboot(0);
}

void serial_cb(const struct device *dev, void *user_data)
{
	int rx_recv = 0;
	struct rx_buf_t *rx = (struct rx_buf_t *)user_data;
	char *rx_buf = rx->buf;
	size_t *rx_buf_pos = &rx->pos;

	if (!uart_irq_update(uart_dev)) {
		return;
	}

	while (uart_irq_rx_ready(uart_dev)) {
		rx_recv = uart_fifo_read(uart_dev, &rx_buf[*rx_buf_pos], 1);

		if ((rx_buf[*rx_buf_pos] == '\n') || (rx_buf[*rx_buf_pos] == '\r')) {
			rx_buf[*rx_buf_pos] = 0;
			k_sem_give(&profile_received);
			return;
		}

		*rx_buf_pos += rx_recv;
	}
}

static int provision_softsim_from_serial(void)
{
	if (!device_is_ready(uart_dev)) {
		LOG_ERR("UART device not found!");
		return -1;
	}

	char *profile = k_malloc(PROFILE_MAX_SIZE);
	__ASSERT_NO_MSG(profile != NULL);

	struct rx_buf_t rx = {
		.buf = profile,
		.len = PROFILE_MAX_SIZE,
		.pos = 0,
	};

	uart_irq_callback_user_data_set(uart_dev, serial_cb, &rx);
	uart_irq_rx_enable(uart_dev);

	do {
		LOG_INF("Transfer SoftSIM profile using serial COM port, terminate by "
			"newline character (return key)");
	} while (k_sem_take(&profile_received, K_SECONDS(20)));

	LOG_INF("Profile received: %zu characters in total", rx.pos);

	uart_irq_rx_disable(uart_dev);

	/* Provision the profile to the SoftSIM filesystem */
	if (nrf_softsim_provision((uint8_t *)profile, rx.pos) != 0) {
		LOG_ERR("SoftSIM Profile provisioning failed");
	}

	k_free(profile);

#ifndef CONFIG_SOFTSIM_FACTORY_RESET_ON_PROVISION
	/* Reboot to free the UART for the shell and bring the modem up cleanly
	 * with the new SIM. With factory reset enabled we must NOT reboot here:
	 * the modem is still uninitialised (AT commands return -NRF_EPERM, i.e.
	 * -1), so the reset waits until main() has called nrf_modem_lib_init() —
	 * see nrf_softsim_just_provisioned(). */
	drain_logs_and_reboot();
#endif /* !CONFIG_SOFTSIM_FACTORY_RESET_ON_PROVISION */

	return 0;
}

int main(void)
{
	LOG_INF("SoftSIM Memfault sample started.");

	if (!nrf_softsim_check_provisioned()) {
		if (provision_softsim_from_serial() != 0) {
			return -1;
		}
	}

	int32_t err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("Failed to initialize modem library, error: %d", err);
	}

#ifdef CONFIG_SOFTSIM_FACTORY_RESET_ON_PROVISION
	/* Modem is now initialised; if a profile was just provisioned (static or
	 * serial), wipe modem NVM and reboot so it comes up clean with the new SIM. */
	if (!err && nrf_softsim_just_provisioned()) {
		nrf_softsim_modem_factory_reset();
		drain_logs_and_reboot();
	}
#endif /* CONFIG_SOFTSIM_FACTORY_RESET_ON_PROVISION */

	modem_connect();

	LOG_INF("Waiting for LTE connect event.");

	/* Post on every (re)registration. Periodic upload keeps running in the
	 * background between events. */
	while (1) {
		do {
		} while (k_sem_take(&lte_connected, K_SECONDS(10)));

		LOG_INF("LTE connected!");
		memfault_post_on_connect();
	}
}
