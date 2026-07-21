# SoftSIM Memfault sample

The `softsim_external_profile` sample plus [Memfault](https://memfault.com),
used as a bug hunter for [onomondo-uicc](https://github.com/onomondo/onomondo-uicc):
the device attaches over LTE with SoftSIM and continuously reports to Memfault

- coredumps for any crash or assert (posted right after the next LTE attach),
- a `softsim_uicc_error` trace event for every onomondo-uicc `SS_LOGP` error
  (via `CONFIG_SOFTSIM_MEMFAULT_TRACE`), with the file:line message attached,
- LTE and stack metrics, plus the most recent Zephyr logs.

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

Note: the Memfault root CAs are provisioned to modem sec tags 1001-1005 on
first boot; HTTPS uploads fail if other certificates already occupy those tags.
