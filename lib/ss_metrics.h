/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Onomondo ApS
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * No-op-able wrappers for Memfault heartbeat metrics so call sites carry no
 * #ifdef litter. Keys are defined by the application; see the
 * CONFIG_SOFTSIM_MEMFAULT_METRICS Kconfig help.
 */
#pragma once

#ifdef CONFIG_SOFTSIM_MEMFAULT_METRICS
#include <memfault/metrics/metrics.h>
#define SS_METRIC_ADD(key, n)      MEMFAULT_METRIC_ADD(key, n)
#define SS_METRIC_TIMER_START(key) MEMFAULT_METRIC_TIMER_START(key)
#define SS_METRIC_TIMER_STOP(key)  MEMFAULT_METRIC_TIMER_STOP(key)
#else
#define SS_METRIC_ADD(key, n)
#define SS_METRIC_TIMER_START(key)
#define SS_METRIC_TIMER_STOP(key)
#endif
