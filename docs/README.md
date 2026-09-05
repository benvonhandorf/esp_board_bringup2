# docs/

Two kinds of page live here.

**The command reference** — one page per group, every command documented. The
conventions that apply everywhere (how commands are addressed, case folding,
`help`, the `ERR:` prefix) are in the [project README](../README.md#the-shell).

| Group | Covers | Related groups |
|---|---|---|
| [sys](sys.md) | Device information, restart, and OTA confirm/rollback | |
| [net](net.md) | The configured link, and the services over it | `wifi` |
| [wifi](wifi.md) | The radio itself: scanning, ad-hoc joins, throughput, powering it down | `net` |
| [gpio](gpio.md) | Digital and analog pin access. `<pin>` accepts `4`, `0-5` or `1,4,8-10` | `gpio-pwm` |
| [i2c](i2c.md) | I2C master, and every part that hangs off it | `i2c-ina219`, `i2c-ina226`, `i2c-ina237`, `i2c-sht4x`, `i2c-nau7802`, `i2c-lm75bdp`, `i2c-rx8130ce`, `i2c-aw9523b`, `i2c-pi4ioe` |
| [uart](uart.md) | Auxiliary UART (separate from this console) | |
| [spi](spi.md) | SPI master | |
| [sd](sd.md) | SD/MMC cards over SPI, 1-bit or 4-bit SD, with speed testing | |
| [audio](audio.md) | Audio over I2S: tone, sweep, microphone capture and loopback | `audio-nau8822`, `audio-ns4168`, `audio-sph0645` |
| [touch](touch.md) | Capacitive touch pads: calibrate a set and watch them live | |
| [loadcell](loadcell.md) | HX711 24-bit load cell ADC, bit-banged on two pins (no bus) | |
| [board](board.md) | Known board pinouts and per-subsystem setup presets | `board-<name>` |

Descriptions match the one-liners the device itself prints for `help`; they are
the `.help` strings in `main/app_console.c`.

**How this project is put together** — for changing it rather than using it.

- [Bringing up a new board](new-board.md) — the running order to follow on a
  board nobody has powered before: survey the pins, then the buses, then each
  part in turn.
- [Bring-up order](startup.md) — what `app_main()` does, and why the order is
  what it is.
- [Configuration](configuration.md) — how one schema becomes the parsers, and how
  to add a section.
- [Updating firmware](ota.md) — flashing, OTA, rollback, and the failure modes.

The shared components document themselves in
`esp_components/<component>/README.md`; what is here covers what belongs *in this
repository*.

## Where to start on an unknown board

[Bringing up a new board](new-board.md) is the guide: which tool to reach for in
which order, and what each stage can prove. In short, [`gpio
survey`](gpio.md#survey-pin) first — it measures every pin and reports what each
one looks like, without you having to know the pinout. [`board
list`](board.md#list) then says whether this is a board the firmware already has
a pinout for.

Which parts have been exercised against real hardware, and which are written but
unverified, is tracked in [HardwareSupport.md](../HardwareSupport.md).

Keep documentation beside the code and change it in the same commit. That is why
it lives in the repository rather than a wiki.
