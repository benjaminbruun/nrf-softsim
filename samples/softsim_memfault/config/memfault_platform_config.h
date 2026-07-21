/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Onomondo ApS
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Overrides for the memfault-firmware-sdk defaults; see
 * components/include/memfault/default_config.h in the SDK.
 */

#pragma once

/* softsim_uicc_error trace logs are file:line-prefixed SS_LOGP messages; the
 * default cap of 80 chars would cut most of them off. */
#define MEMFAULT_TRACE_EVENT_MAX_LOG_LEN 200
