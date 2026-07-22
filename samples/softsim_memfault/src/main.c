/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-2026 Onomondo ApS
 * SPDX-License-Identifier: GPL-3.0-only
 */

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
#include <memfault/core/trace_event.h>
#include <memfault/metrics/metrics.h>
#include <memfault/panics/coredump.h>
#include <memfault/ports/watchdog.h>
#include <memfault/ports/zephyr/http.h>

LOG_MODULE_REGISTER(softsim_sample, LOG_LEVEL_INF);

/* Headroom over the full SoftSIM profile (~410 chars incl. SMSP/PIN/SMSC) */
#define PROFILE_MAX_SIZE 512

/* Reconnect-churn soak cadence. Each cycle: dwell online, then force a
 * re-attach that powers the UICC off/on (CFUN=4 shuts the UICC down on nRF91,
 * so every cycle re-runs SoftSIM DEINIT -> INIT/ATR/USIM file re-reads, plus
 * network re-authentication whenever the network challenges).
 * ponytail: compile-time knobs on purpose - anyone running a soak rig is
 * already editing and rebuilding this sample; upgrade path is a sample Kconfig. */
#define DWELL_ONLINE_SEC	300 /* one heartbeat/upload period online per cycle */
#define OFFLINE_HOLD_SEC	5
#define CYCLES_PER_FULL_RESTART 10  /* deep modem-lib restart every Nth cycle */
#define CONNECT_TIMEOUT_SEC	180
#define MAX_CONNECT_FAILURES	3   /* then reboot; Memfault records the reboot reason */
#define WATCHDOG_FEED_SLICE_SEC 10  /* < CONFIG_MEMFAULT_SOFTWARE_WATCHDOG_TIMEOUT_SECS */

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

/* Deepest restart short of a reboot: takes the whole modem library down and
 * back up, re-running the NRF_MODEM_LIB_ON_INIT SoftSIM hooks (SIM select,
 * handler registration) in addition to the UICC power cycle. Returns 0 when
 * the modem is up and reconnecting. */
static int full_modem_restart(void)
{
	int err;

	LOG_INF("Full modem library restart");

	(void)lte_lc_power_off();

	err = nrf_modem_lib_shutdown();
	if (err) {
		LOG_ERR("Modem library shutdown failed, error: %d", err);
	}

	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("Modem library init failed, error: %d", err);
		return err;
	}

	modem_connect();

	return 0;
}

/* Block until registered or timeout, feeding the software watchdog. */
static bool wait_registered(void)
{
	for (int waited = 0; waited < CONNECT_TIMEOUT_SEC; waited += WATCHDOG_FEED_SLICE_SEC) {
		memfault_software_watchdog_feed();
		if (k_sem_take(&lte_connected, K_SECONDS(WATCHDOG_FEED_SLICE_SEC)) == 0) {
			return true;
		}
	}

	return false;
}

/* Stay online for the dwell period, feeding the software watchdog. Memfault
 * periodic upload/heartbeat traffic keeps flowing in the background. */
static void dwell_online(void)
{
	for (int slept = 0; slept < DWELL_ONLINE_SEC; slept += WATCHDOG_FEED_SLICE_SEC) {
		memfault_software_watchdog_feed();
		k_sleep(K_SECONDS(WATCHDOG_FEED_SLICE_SEC));
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

	/* ponytail: a hard fault with interrupts wedged escapes a software
	 * watchdog, but Memfault's fault handlers already cover hard faults;
	 * upgrade path is the hardware WDT. */
	memfault_software_watchdog_enable();

	modem_connect();

	LOG_INF("Waiting for LTE connect event.");

	/* Reconnect-churn soak loop: dwell online, then force a re-attach that
	 * power-cycles the UICC, forever. Cycle/failure stats land in the
	 * lte_churn_* heartbeat metrics; the SoftSIM lib's softsim_* metrics
	 * show the resulting UICC traffic. */
	int fail_streak = 0;

	for (uint32_t cycle = 0;; cycle++) {
		if (!wait_registered()) {
			LOG_ERR("Not registered within %d s", CONNECT_TIMEOUT_SEC);
			MEMFAULT_METRIC_ADD(lte_churn_fail_count, 1);
			MEMFAULT_TRACE_EVENT(lte_reattach_timeout);

			if (++fail_streak >= MAX_CONNECT_FAILURES) {
				LOG_ERR("%d consecutive attach failures, rebooting",
					fail_streak);
				drain_logs_and_reboot();
			}

			/* Recovery attempt doubles as the deep churn path. */
			full_modem_restart();
			continue;
		}

		fail_streak = 0;
		/* Started at the previous churn; errors out harmlessly on the
		 * first (boot) registration where no churn preceded it. */
		MEMFAULT_METRIC_TIMER_STOP(lte_churn_reattach_ms);
		MEMFAULT_METRIC_ADD(lte_churn_cycle_count, 1);

		LOG_INF("LTE connected! Churn cycle %u", cycle);
		memfault_post_on_connect();

		dwell_online();

		/* Churn: drop a stale registration signal, time the re-attach. */
		k_sem_reset(&lte_connected);
		MEMFAULT_METRIC_TIMER_START(lte_churn_reattach_ms);

		if ((cycle + 1) % CYCLES_PER_FULL_RESTART == 0) {
			full_modem_restart();
		} else {
			LOG_INF("Going offline (UICC off)");
			(void)lte_lc_offline(); /* CFUN=4: SoftSIM DEINIT */
			k_sleep(K_SECONDS(OFFLINE_HOLD_SEC));
			(void)lte_lc_normal(); /* CFUN=1: SoftSIM INIT/ATR + re-attach */
		}
	}
}
