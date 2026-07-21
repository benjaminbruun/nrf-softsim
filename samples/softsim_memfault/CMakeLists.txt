# SPDX-FileCopyrightText: Copyright (c) 2023-2026 Onomondo ApS
# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.20.0)

list(APPEND OVERLAY_CONFIG "$ENV{ZEPHYR_BASE}/../modules/lib/onomondo-softsim/overlay-softsim.conf")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(softsim_memfault)

# Memfault user config (trace reasons, heartbeat metrics, platform overrides)
zephyr_include_directories(config)

target_sources(app PRIVATE src/main.c)
# Battery SoC via ADP536x fuel gauge; enabled per-board (see boards/thingy91_nrf9160_ns.conf)
target_sources_ifdef(CONFIG_MEMFAULT_METRICS_BATTERY_ENABLE app PRIVATE src/battery.c)
