# SoftSIM Memfault sample

A long-running SoftSIM stress/soak firmware with [Memfault](https://memfault.com)
as the telemetry backend, used as a bug hunter for
[onomondo-uicc](https://github.com/onomondo/onomondo-uicc): the device stays
online and awake (LTE-M only, no PSM/eDRX) and runs a reconnect-churn loop
that keeps hammering the SoftSIM↔modem integration while reporting to Memfault

- coredumps for any crash or assert (posted right after the next LTE attach),
- a `softsim_uicc_error` trace event for every onomondo-uicc `SS_LOGP` error
  (via `CONFIG_SOFTSIM_MEMFAULT_TRACE`), with the file:line message attached,
- SoftSIM operation heartbeat metrics (via `CONFIG_SOFTSIM_MEMFAULT_METRICS`)
  plus churn-loop metrics from the app itself,
- LTE and stack metrics, plus the most recent Zephyr logs (SoftSIM modules at
  full debug verbosity, uploaded on every periodic pass).

## Churn loop

After each dwell period online, `src/main.c` forces a full re-attach with
`lte_lc_offline()` → `lte_lc_normal()`. CFUN=4 powers the UICC off on nRF91,
so every cycle re-runs SoftSIM DEINIT, then INIT/ATR and the USIM file
re-reads, plus network re-authentication (MILENAGE) whenever the network
challenges. Every `CYCLES_PER_FULL_RESTART`th cycle the whole modem library
is shut down and re-initialised instead — the deepest restart short of a
reboot, re-running the SoftSIM `NRF_MODEM_LIB_ON_INIT` hooks.

Cadence is set by the `#define`s at the top of `src/main.c` (dwell 300 s,
offline hold 5 s, full restart every 10th cycle, 180 s attach timeout).
Failure handling: an attach timeout counts into `lte_churn_fail_count`, emits
a `lte_reattach_timeout` trace event, and is retried with a full modem
restart; 3 consecutive failures reboot the device. A Memfault software
watchdog (120 s, fed by the loop) turns a hung loop into a coredump + tracked
reboot instead of a silent wedge.

Per heartbeat, expect `softsim_deinit_count`/`softsim_init_count` ≈
`lte_churn_cycle_count`, and `softsim_auth_count` to track it only when the
network actually re-authenticates — a flat `auth_count` under growing cycle
counts means the network is reusing the cached EPS NAS security context,
which is itself a soak finding, not a bug.

## SoftSIM metrics

Defined in `config/memfault_metrics_heartbeat_config.def`, incremented by the
nrf-softsim glue layer (the onomondo-uicc library is untouched), reset per
heartbeat (5 min in this sample):

| Metric | Meaning |
|---|---|
| `softsim_apdu_count` / `softsim_apdu_err_count` | APDUs from the modem / responses with an error-class SW1 (not 90/91/92/61/62/63/9F per TS 102 221) |
| `softsim_auth_count` / `softsim_auth_err_count` | AUTHENTICATE (INS `0x88`) subset; AUTS sync failure counts as success |
| `softsim_apdu_active_ms` | total time inside `ss_application_apdu_transact()`; avg latency = active_ms / apdu_count |
| `softsim_init/reset/deinit/suspend_count` | modem-driven lifecycle requests |
| `softsim_nvs_read/write_count`, `_bytes`, `softsim_nvs_delete_count` | NVS flash traffic |
| `softsim_fs_cache_hit` / `softsim_fs_cache_miss` | filesystem cache effectiveness on `ss_fopen` |
| `softsim_uicc_err_count` (+ `_auth`, `_fs`, `_apdu` buckets) | onomondo-uicc `SS_LOGP` error volume by subsystem group |
| `lte_churn_cycle_count` / `lte_churn_fail_count` | completed churn cycles / attach timeouts (from `src/main.c`) |
| `lte_churn_reattach_ms` | total offline→registered time; avg reattach = reattach_ms / cycle_count |

Provisioning works exactly like `softsim_external_profile`: transfer the
profile over serial on first boot (or build with `overlay-static.conf`).

## Build

The Memfault project key is not committed - inject it at build time
(project keys identify, they don't authenticate, but this repo is public):

```sh
west build --sysbuild -b nrf9151dk/nrf9151/ns . -- \
  -DCONFIG_MEMFAULT_NCS_PROJECT_KEY=\"<your project key>\"
```

Then flash `build/merged.hex` and upload the symbol file
`build/softsim_memfault/zephyr/zephyr.elf` to Memfault
(**Software → Symbol Files**) so coredumps and traces symbolicate.

### Thingy:91

On a Thingy:91 the sample additionally reports battery metrics
(`battery_soc_pct` etc.) from the ADP536x fuel gauge, via
`boards/thingy91_nrf9160_ns.conf` and `src/battery.c`:

```sh
west build --sysbuild -b thingy91/nrf9160/ns . -- \
  -DSB_CONFIG_THINGY91_NO_PREDEFINED_LAYOUT=y \
  -DSB_CONFIG_BOOTLOADER_MCUBOOT=y \
  -DSB_CONFIG_MCUBOOT_MODE_SINGLE_APP=y \
  -DCONFIG_MEMFAULT_NCS_PROJECT_KEY=\"<your project key>\"
```

The app doesn't fit the 192 KB the factory dual-slot layout leaves it, so
`pm_static_thingy91_nrf9160_ns.yml` keeps MCUboot and the primary slot at the
factory bootloader's addresses but gives the secondary slot's space to the
app and moves the storage partitions out of the slot. No FOTA/swap updates,
and in return the SoftSIM profile and stored coredumps survive reflashes.

Flash over USB, no debugger needed: power off, hold the center button (SW3)
while switching power on - the device enumerates as MCUboot serial recovery -
then:

```sh
nrfutil device program --firmware build/dfu_application.zip --traits mcuBoot
```

Images are signed with the default NCS dev key (`root-ec-p256.pem`), which
the preloaded factory bootloader accepts. If it doesn't boot (LED stays off,
re-enumerates as MCUboot), the bootloader was built with a different key:
flash `build/merged.hex` once with a debugger, which installs a matching
MCUboot, and USB recovery works from then on.

### Thingy:91 X

Builds with the plain nrf9151dk command, just with `-b thingy91x/nrf9151/ns` -
the repo-level `boards/thingy91x_nrf9151_pm_static.yml` already provides a
factory-bootloader-compatible layout, so the same serial-recovery flash flow
applies (power off, hold the button while switching on, then program
`dfu_application.zip` with `--traits mcuBoot`). No battery metrics here yet:
the Thingy:91 X has an nPM1300 PMIC, not the ADP536x this sample's
`battery.c` drives.

## Verify the integration

On the shell (UART, 115200):

```
mflt get_device_info          # device ID (IMEI), fw version
mflt test trace               # capture a test trace event
mflt test heartbeat           # close out a heartbeat
mflt post_chunks              # push buffered data now
mflt test assert              # crash -> coredump -> reboot -> auto-upload on attach
mflt coredump_size            # sanity-check coredump fits the flash partition
```

A successful upload logs an HTTP 202 response. Without network coverage,
`mflt export` prints base64 chunks that can be forwarded with the
[Memfault CLI](https://docs.memfault.com/docs/mcu/export-chunks-over-console).

To exercise the UICC error path end-to-end, send a malformed APDU to the
SoftSIM (e.g. `at AT+CSIM=10,"0080000000"`) and watch for the
`softsim_uicc_error` trace event in **Issues** after the next upload.

### Verify the metrics

After an attach plus one heartbeat (or `mflt test heartbeat` + `mflt
post_chunks`), the device's **Metrics** tab should show `softsim_apdu_count`
in the tens-to-hundreds, `softsim_auth_count >= 1`, a nonzero
`softsim_apdu_active_ms`, and cache/NVS counters. Once the churn loop is
running, `lte_churn_cycle_count` grows by roughly one per heartbeat (with the
default 300 s dwell) with `lte_churn_fail_count` staying 0, and the
`softsim_init/deinit_count` lifecycle counters tick along with it. Error counters stay 0 in
steady state; the malformed-APDU test above bumps `softsim_apdu_err_count`
and the `softsim_uicc_err_*` buckets. **Logs** should show `softsim` and
`softsim_uicc` lines at debug level.

Note: the Memfault root CAs are provisioned to modem sec tags 1001-1005 on
first boot; HTTPS uploads fail if other certificates already occupy those tags.
