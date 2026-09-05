# AGENTS.md

Orientation for anyone — human or agent — working in this project. Read this
before changing code; it should save most of a codebase scan. `docs/` is the
user-facing manual, one page per group with every command documented, so consult
`docs/<group>.md` rather than reverse-engineering behaviour from source.
`README.md` is the front door.

## What this is

An ESP-IDF application that turns any ESP32-series board into a CLI-driven
bring-up rig, built from the `idf_template` template. The parts every project
needs — configuration, WiFi, mDNS, NTP, MQTT, the command shell, HTTP, OTA — and
every device driver live in **shared components** in a separate repository
(`esp_components`), fetched by tag; see `main/idf_component.yml`. What is *here*
is the part specific to a bring-up rig: the commands.

That split is the whole design. If you find yourself editing a shared component
to make this project work, ask first whether the component is missing a general
capability or whether this project is reaching through an abstraction it should
be using. `wifi_manager_scan()` exists because the answer was the former — see
"Things that will bite".

ESP-IDF **v6.0.2**. C11, `-Wall -Wextra -Werror` on `main`.

Targets built against: **ESP32-C3** (the sensor board on the bench) and
**ESP32-S3**; several board presets are for the ESP32.

## Layout

```
config/config_schema.json   the ONE description of this device's configuration
fs/                         LittleFS image contents; config.json is gitignored
main/
  main.c                    app_main: bring-up order, and the reasons for it
  app_console.c             THE wiring point: every group and command is here
  config_reader.{c,h}       loads the config; dispatches sections to their owners
  app_status.{c,h}          what this device reports
  app_http.{c,h}            this project's HTTP routes
  app_bringup.h             the includes every command module wants
  audio/                    i2s_bus.c (transport), tone.c (signal generation),
                            capture.c (input statistics, FFT, tone detection),
                            audio.c (the group + codec registry), codec_*.c
  board/board.c             named pinouts and per-subsystem setup presets
  gpio/                     gpio.c (set/read/aread/blink/short/rc/survey), pwm.c
  i2c/                      i2c.c (bus/scan/read) plus the console half of each
                            part: ina219_cmd.c, ina226_cmd.c, ina237_cmd.c,
                            sht4x_cmd.c, nau7802_cmd.c, lm75bdp_cmd.c,
                            rx8130ce_cmd.c, aw9523b_cmd.c, pi4ioe5v6408_cmd.c
  loadcell/hx711_cmd.c      the console half; the driver is the hx711 component
  sd/sd.c                   SD over SPI / 1-bit / 4-bit, benchmarks, clock sweep
  spi/spi.c                 SPI master
  sys/sys_hw.c              the chip half of `sys`: chip info, lfxtal
  touch/touch.c             capacitive touch pads
  uart/uart.c               auxiliary UART, separate from the console
  wifi/wifi.c               the bring-up half of WiFi, layered on wifi_manager
partitions/                 A/B OTA layouts for 4/8/16 MB
tools/                      flashing, OTA upload, status, MQTT tail, console
                            driver (bringup.py), docs check (check_docs.py)
```

**The subdirectories under `main/` are plain source folders, not ESP-IDF
components.** IDF only auto-discovers components under `components/` or
`EXTRA_COMPONENT_DIRS`, so an `idf_component_register()` in `main/gpio/` would be
silently ignored. **A new file must be added to `SRCS` in `main/CMakeLists.txt`**
or it is never compiled, silently.

## How a command works

1. The serial reader (in the `cli` component) or `cli_web` receives a line. Both
   call `cli_submit()`, which queues it for a single executor task — so commands
   never run concurrently and modules need no locking of their own.
2. `cli_execute()` resolves `<group> <command>` against the registered groups,
   case-folding *command tokens only* (argument values keep their case).
3. The command receives `argv[0] == its own name`. So `gpio set 19 true` reaches
   `cmd_gpio_set()` as `argc=3`, `argv={"set","19","true"}`.

**Every command line is exactly two tokens.** There is no current group and no
way to stand inside one; depth is a hyphenated group name (`i2c-nau7802`,
`gpio-pwm`), never a third token. See the note at the top of `cli.h` for the two
failure modes the nested menus this replaced brought with them.

## Adding things

**A command.** Implement `int cmd_<module>_<name>(int argc, char **argv)` in the
module's `.c`, declare it in the module's `.h`, add a `cli_command_t` row to the
group in `app_console.c`, and document it under a matching heading in
`docs/<group>.md`. `./tools/check_docs.py` fails if you skip the last step.

**A group.** A `cli_command_t[]` and a `cli_group_t` in `app_console.c`, a row in
the `groups[]` array, and an entry in `PAGE` in `tools/check_docs.py` naming the
docs page that covers it.

**A device driver.** It goes in `esp_components`, not here. What lives here is
the `_cmd.c` shim: argument parsing, which parts the user has configured, and
every line of text. The component must never include `diag.h` or format prose —
verify with `grep -rn "diag_printf\|diag_error" ../esp_components/<name>/`, which
must be empty.

**A configuration section.** Add a `$ref` to `config/config_schema.json` naming
the component that owns the schema, add the field to `app_config_full_t` and a
`SECTION_PARSER_<name>` line in `config_reader.c`. Forgetting the last step does
not compile.

**An HTTP route.** A `http_route_t` row in `app_http.c`. Set `require_auth` on
anything that exposes credentials or changes the device.

## Conventions

- **Never `printf()`.** Use `diag_printf()`, or output never reaches the web
  console. `ESP_LOGx` is already routed through the same fan-out.
- **Report failures with `diag_error()`** — it prefixes `ERR:` so a host script
  can detect failure without parsing prose — and return `-1`; return `0` on
  success. Usage complaints are printed as `Usage: ...`.
- **Parse arguments with `cli_parse_*`**, never `atoi()`. They consume the whole
  token, so `gpio read foo` is an error rather than a read of pin 0.
- **A message that names a command must name it in full.** `run 'tare' first` was
  correct when you could stand inside a menu and is wrong now; it has to be `run
  'i2c-nau7802 tare' first`. This is the single most common way to break the
  manual and the error messages at once, and it is invisible until someone is
  already stuck. `tools/check_docs.py` catches the docs half.
- **`main` must never set `REQUIRES` or `PRIV_REQUIRES`.** `project.cmake` grants
  it an implicit dependency on every component in the build *only* while both are
  unset; naming one silently strips the rest.
- Output is line-oriented and script-friendly: one record per line.

## Things that will bite

- **`idf.py flash` is not enough.** It writes `ota_0` while the bootloader
  follows `otadata`, so after an OTA a plain flash leaves the device running the
  old firmware, silently. Use `tools/flash_a.py`.
- **Do not call `esp_wifi_scan_start()` from this project.** `wifi_manager` owns
  the WiFi driver and its `WIFI_EVENT_SCAN_DONE` handler consumes or clears the
  records before a blocking scan started here returns — the command reports no
  access points while the manager's log names three. Use `wifi_manager_scan()`.
  The same reasoning covers the rest of the radio: ask the manager to change
  state, do not reach around it.
- **One SPI host.** `BP_SPI_HOST_ID` (`SPI2_HOST`) is shared by the `spi` and
  `sd` groups; on the C3 it is the only general-purpose host. Each refuses rather
  than stealing it, via the mirrored `spi_group_owns_host()` and
  `sd_owns_spi_host()`.
- **SD host peripheral is chip-dependent.** `sd mmc` is behind
  `SOC_SDMMC_HOST_SUPPORTED`; it exists on the S3 and not on the C3.
- **SD bring-up is deliberately two-stage.** `sd spi` / `sd mmc` init the host and
  card; FAT is mounted only when `sd bench` needs it. A blank or non-FAT card must
  still report its identity and still be measurable.
- **`audio/i2s_bus.c` is the only file that includes `driver/i2s_*.h`**, which is
  what keeps the `#if SOC_I2S_SUPPORTED` guard to one file; the `#else` half
  provides stubs so everything above builds on a chip with no I2S.
- **Two codecs attach at once, one per direction.** `detach()` powers a part down,
  so a single slot would make attaching a microphone silently stop the amplifier.
- **`audio bus` leaves the transmitter running**, sending silence. Codecs mute or
  reset when their clocks stop, and cycling the clock around every tone pops an
  amplifier.
- **A supplied scale factor is gain-specific**, in both load-cell drivers, and
  `weight` refuses when a factor is set with no tare. See `docs/i2c.md` and
  `docs/loadcell.md`; both explain why, and both are easy to undo by accident.
- **The image is on trial after an OTA.** `app_main()` confirms it at the very
  end, so firmware that crashes during start-up rolls back on the next reset. Do
  not move that call earlier.
- **A shared component is pinned to a git tag, not a version range.** To work on a
  component and this project together, build with
  `-DESP_COMPONENTS_DIR=/abs/path` — use an **absolute** path, since CMake
  resolves a relative one against the build directory. That build rewrites
  `dependencies.lock` with local paths; committing it would un-pin the project for
  everyone. `idf.py reconfigure` puts the pins back — check `git diff` before
  committing after working that way. Anything changed that way has to be committed
  and tagged in `esp_components`, **and pushed**, before a plain `idf.py build`
  resolves it.
- **`dependencies.lock` is committed and target-specific.** `idf.py set-target`
  rewrites it. That churn belongs in the commit.
- **`sdkconfig` is generated and gitignored.** Edit `sdkconfig.defaults` or
  `sdkconfig.defaults.<target>`, then `idf.py reconfigure`.
- **`.vscode/settings.json` sets `IDF_TARGET` as an environment variable**, which
  outranks everything else. Build from a plain shell to avoid a cache mismatch.
- **Flash is getting tight.** The image is ~1.5 MB against a 1.69 MB OTA slot on
  4 MB parts. Adding another large subsystem may need the 512 kB `res` partition
  shrunk, or an 8 MB layout.

## Verifying a change

```sh
idf.py build                       # -Werror is on for main
./tools/check_docs.py              # commands and the manual agree
./tools/flash_a.py -p /dev/ttyACM0
./tools/bringup.py "sys info" "i2c scan"
```

There is no test suite here; the components carry host tests for the arithmetic
that fails silently (`make -C ../esp_components/js2c/test`). Verification of this
project is a build, the docs check, and running commands on hardware.
