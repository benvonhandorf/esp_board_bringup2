# ESP Board Bringup

A CLI-driven way to exercise any ESP32-series board before a BSP exists. Drive
GPIO, I2C, SPI, UART, SD, I2S audio, touch and WiFi from a shell, on a board
whose pinout you may not know yet — `gpio survey` will tell you a good deal of
it.

The shell is reachable over the primary serial port **and** over a web page the
device hosts, at the same time. Output from a command goes to both, whichever one
typed it.

Built from [idf_template](https://github.com/benvonhandorf/idf_template), so the
parts every project needs — configuration, WiFi, mDNS, NTP, MQTT, the shell,
HTTP, OTA — come from shared components in
[esp_components](https://github.com/benvonhandorf/esp_components), fetched by
tag. What is in this repository is the bring-up rig itself.

## Building and running

```sh
idf.py set-target esp32c3          # or esp32s3, esp32; first time, and to switch
cp config/config.example.json fs/config.json      # then edit it
idf.py build
./tools/flash_a.py -p /dev/ttyACM0
```

`idf.py flash` is **not** enough after an over-the-air update: it writes `ota_0`
while the bootloader follows `otadata`, so the device comes back running the old
firmware, silently. `tools/flash_a.py` erases `otadata` as part of flashing. See
[Updating firmware](docs/ota.md).

`sdkconfig` is generated and gitignored; edit `sdkconfig.defaults` or
`sdkconfig.defaults.<target>` and run `idf.py reconfigure`. One caveat when
switching targets: `.vscode/settings.json` sets `IDF_TARGET` as an environment
variable, which outranks everything else and fails the build with a CMake cache
mismatch. Update it to match, or build from a plain shell.

The console is the chip's **primary** serial device. On boards with a native
USB-Serial-JTAG port (which enumerate as `/dev/ttyACM*`) that must be
`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`; a secondary console is output-only and
would never receive your keystrokes. Line editing, history and tab completion
come from linenoise and appear once a terminal is attached.

## The shell

Every command is exactly **two tokens**: a group and a command.

```
gpio set 19 true
i2c-nau7802 read
audio-nau8822 volume 50
```

Groups are a naming, help and completion device only. There is no current group,
no prompt path, and no `back` or `exit` — you do not enter a group, you address
one. Depth is spelled by hyphenating the group name, which is why every command
line is two tokens regardless of how the hardware is organised.

That is a deliberate reversal of the nested menus this project used to have.
Navigation cost around 200 lines of state and brought two failure modes with it:
a command in the current menu could shadow a same-named command at the root
depending on where you happened to be standing, and a command line composed by
the firmware itself could be captured by the very command that was running it,
recursing without bound. A board preset did exactly that.

`help` lists the groups; `help <group>` lists one group's commands; typing a
group name alone does the same. Commands are case-insensitive, but only the
command tokens are case-folded — arguments such as WiFi passwords and UART
payloads keep their case exactly as typed.

Failures are reported as a line beginning with `ERR:`, so a host script can tell
success from failure without parsing prose. `tools/bringup.py` drives the console
over the serial port and is importable by test scripts.

### The groups

Each has its own page in **[docs/](docs/README.md)**.

| Group | Covers | Related |
|---|---|---|
| [sys](docs/sys.md) | Device information, restart, OTA confirm/rollback | |
| [net](docs/net.md) | The configured link and the services over it | |
| [wifi](docs/wifi.md) | The radio itself: scanning, ad-hoc joins, throughput, powering it down | |
| [gpio](docs/gpio.md) | Digital and analog pin access. `<pin>` accepts `4`, `0-5` or `1,4,8-10` | `gpio-pwm` |
| [i2c](docs/i2c.md) | I2C master, and every part on it | `i2c-ina219`, `i2c-ina226`, `i2c-ina237`, `i2c-sht4x`, `i2c-nau7802`, `i2c-lm75bdp`, `i2c-rx8130ce`, `i2c-aw9523b`, `i2c-pi4ioe` |
| [uart](docs/uart.md) | Auxiliary UART (separate from this console) | |
| [spi](docs/spi.md) | SPI master | |
| [sd](docs/sd.md) | SD/MMC over SPI, 1-bit or 4-bit SD, with speed testing | |
| [audio](docs/audio.md) | Audio over I2S: tone, sweep, capture, loopback | `audio-nau8822`, `audio-ns4168`, `audio-sph0645` |
| [touch](docs/touch.md) | Capacitive touch pads | |
| [loadcell](docs/loadcell.md) | HX711 24-bit load cell ADC, bit-banged (no bus) | |
| [board](docs/board.md) | Known board pinouts and per-subsystem presets | `board-<name>` |

On a board nobody has powered before, **[Bringing up a new
board](docs/new-board.md)** is the running order: survey the pins, then the
buses, then each part in turn. It starts with [`gpio
survey`](docs/gpio.md#survey-pin), which measures every pin and reports what each
looks like before you have a pinout. What has been verified against real hardware
is tracked in [HardwareSupport.md](HardwareSupport.md).

## Web interface

Once the device has an address, a single-page console is served on port 80 and
advertised over mDNS under the `device_name` in `config.json`. Commands typed in
the browser travel over a WebSocket at `/ws`.

The address comes from either mode. As a station it is whatever DHCP handed out,
reported by `wifi status` and `net status`. As an access point it is always
`192.168.4.1`, and the console is up the moment the AP is — there is no lease to
wait for. If mDNS does not resolve (it often will not over an AP the client just
joined), use the numeric address.

The device joins a known network or raises its own access point at boot without
being asked, so a board with no reachable network is serving this page over its
own AP within a few seconds of power-up.

Both interfaces feed the same command queue and share one output fan-out, so
commands run one at a time and every line of output reaches the serial port and
every connected browser — regardless of where the command was entered.

## Where the components come from

`main/idf_component.yml` names each shared component and the tag it is pinned to;
the component manager fetches them on the first build, into `managed_components/`.
There is nothing to clone beside this project.

That includes the device drivers. The INA219, INA226, INA237, SHT4x, NAU7802,
LM75B, RX8130CE, AW9523B, PI4IOE5V6408 and HX711 all live in `esp_components`;
what is here is only the console half of each — argument parsing, which parts the
user has configured, and every line of text. A component returns facts and the
application formats prose, which is what lets the same driver serve a product
that has no shell at all.

`version` is a **git ref**, not a semver range: the tag named is the code you
get, and `dependencies.lock` records what was resolved. Upgrading a component is
editing one line.

To work on a component and this project at the same time, point the build at a
checkout — a local component of the same name wins over the fetched one:

```sh
idf.py -DESP_COMPONENTS_DIR=/abs/path/to/esp_components build
```

Use an absolute path: CMake resolves a relative one against the build directory,
not the source directory. Building this way rewrites `dependencies.lock` to point
at the checkout — do not commit that; a plain `idf.py reconfigure` restores the
pins.

## Configuration

`config/config_schema.json` is the one description of this device's
configuration; the C types, the parsers and the top-level walker are generated
from it. `fs/config.json` holds the real values and is gitignored.

A device with no `config.json` still boots — onto its own access point, with the
console running — which is the right default for a tool carried from board to
board. See [Configuration](docs/configuration.md).

## Layout

```
config/       config_schema.json -- the one authored description; and an example
fs/           contents of the LittleFS partition (config.json is gitignored)
main/
  app_console.c   the wiring point: every group and command is declared here
  <subsystem>/    gpio, i2c, spi, sd, uart, audio, touch, loadcell, board, wifi, sys
partitions/   4 MB / 8 MB / 16 MB A/B layouts
tools/        flashing, OTA upload, status, MQTT tail, console driver, docs check
docs/         the manual: one page per group, plus how to extend the project
```

See [AGENTS.md](AGENTS.md) for the conventions this project follows.
