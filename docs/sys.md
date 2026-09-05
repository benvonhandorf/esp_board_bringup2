# sys

[← Command reference](README.md) · [Project README](../README.md)

Device information and control. This is the bring-up rig's `system` menu merged
with the template's `sys` group: the chip half and the firmware half were two
commands' worth of the same question.

## `info`

Chip model, silicon revision and core count; the radio and flash features fused
into the part; flash size; internal, DMA and PSRAM heap totals with their low
water marks; the station MAC; the reason for the last reset; uptime; and then
the firmware name and version, its build time, the ESP-IDF version and where the
configuration was loaded from.

The last line is worth reading on a device you did not just flash. `Config:
flash` means `fs/config.json` in the LittleFS partition; `Config: sdcard` means a
`config.json` on a mounted card overrode it; `Config: defaults` means neither was
readable and every field is the schema's default — which is why the device is on
its own access point.

## `status`

The JSON status message, exactly as it is published to MQTT and served from
`/api/status`. Printing it here rather than describing it means there is one
definition of what this device reports, in `main/app_status.c`.

## `lfxtal`

Configure the 32.768 kHz crystal oscillator and report whether it is real.

The oscillator is enabled, given time to start, and then *measured*, by
calibrating it against the main crystal. A dead oscillator makes that
calibration time out, which is what distinguishes "no crystal fitted" from a
working one. `rtc_clk_slow_freq_get_hz()` cannot be used for this: it returns a
nominal 32768 for whichever source is selected, regardless of whether anything
is oscillating.

Only if the measurement lands near 32.768 kHz is the RTC slow clock actually
switched over to it. Otherwise the previous source is left alone and the failure
is reported, because a slow clock running from a crystal that is not there is
worse than one running from the internal RC oscillator.

## `restart`

Soft-reset the device. Output is flushed first, so the message reaches the serial
port and every attached browser before the CPU restarts.

## `confirm` and `rollback`

An image installed over the air boots **on trial**. `app_main()` confirms it only
after everything has started, so firmware that crashes during start-up is rolled
back on the next reset — see [Updating firmware](ota.md).

`confirm` marks the running image good by hand, and reports that there was
nothing to confirm if it was already marked. `rollback` returns to the previous
image and reboots, and fails if there is no previous image to return to.
