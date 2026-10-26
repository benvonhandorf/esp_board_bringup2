# Board

[← Command reference](README.md) · [Project README](../README.md)

Known board pinouts, and presets that replay the commands which use them.

Every command in this project takes explicit pin numbers, which is right for a
tool whose job is to find out how a board is wired. Once a board *has* been
worked out, though, retyping its pinout is only a way to make mistakes.

**Naming a board changes nothing.** `board-cardputer` lists what is
available; only `board-cardputer audio` runs anything, and it echoes each line
as it goes, so the preset is a shortcut for the commands rather than a
replacement for knowing them:

```
> board-cardputer audio
> audio bus 41 43 42
I2S initialized on BCLK=41, WS=43, DOUT=42
...
> audio-ns4168 init
NS4168 attached; no SD pin given, so it is assumed hard-enabled
```

## `list`

Lists the known boards: name, the chip it carries, and a one-line description.
Each has its own group, `board-<name>`; typing that group name lists the
subsystems it has presets for.

## `board-<name> pins`

Prints the pinout table. Pins carry a note where they came from a schematic
rather than from having been exercised here — every pin on the Cardputer has now
been used in anger, the microphone's by clocking it from a *different* GPIO and
watching the data go perfectly static, which is what distinguishes "this is the
clock pin" from "the schematic says so". A signal that exists on the board but is
not brought out to a GPIO is listed with a `-` rather than omitted, because
"there is no enable pin" is an answer and a missing row is not.

## `board-<name> <subsystem>`

Runs that subsystem's setup, echoing each line as it goes. A preset is refused
when the firmware is built for a different chip than the board carries, and it
stops at the first line that fails rather than carrying on into commands whose
predecessor did not work.

### board-cardputer

M5Stack Cardputer, ESP32-S3. `pins`, `audio`, `mic`, `sd`, `display`.

**The Cardputer cannot record its own speaker.** GPIO 43 carries the speaker's
word-select *and* the microphone's clock, so only one of the two can have the pad
at a time — `board-cardputer audio` and `board-cardputer mic` are mutually
exclusive, and `audio loopback` has nothing to work with there. `audio pdm`
refuses with that explanation rather than letting the second peripheral quietly
take the pin from the first, which is the failure worth preventing: it is not an
error but a plausible silence, with the amplifier seeing a megahertz square wave
where its frame clock used to be.

### board-cardputer-adv

M5Stack Cardputer Adv, ESP32-S3. `pins`, `audio`.

An ES8311 codec replaces the original's NS4168 amplifier and PDM microphone, so the
speaker and the microphone share one I2S bus and no pin: `audio loopback` and
`audio record` work here, which they cannot on the Cardputer. `board-cardputer-adv audio`
runs `i2c bus 9 8`, `audio bus 41 43 42 din 46` and `audio-es8311 init`.

The codec has no MCLK pin and clocks from the bit clock, so there is no `mclk` argument.
Pass `rate` and `bits` to `audio bus` for anything but the defaults, for example
`audio bus 41 43 42 din 46 rate 8000 bits 16`, then `audio-es8311 init` again.
The pins are from M5Stack's documentation and have not yet been exercised here.

**Build with the no-PSRAM overlay.** The Stamp-S3A has no PSRAM, and the default S3 build
enables octal PSRAM, so a plain build boot-loops with "PSRAM chip is not connected":

```
idf.py -B build_adv -DIDF_TARGET=esp32s3 -DSDKCONFIG=sdkconfig.adv \
  "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.adv" build
```

### board-xiao

Seeed XIAO ESP32-S3 Sense. `pins`, `mic`, `sd`.

No speaker, so there is no audio output preset.

### board-sensor

ESP32-C3 sensor board — INA237 ×2, NAU7802, SHT4x, relay, microSD. `pins`, `i2c`.

`board-sensor i2c` is one line, `i2c bus 5 4`, which is the whole of what this
board needs before its parts answer.

### board-minstro

Minstro ESP32-S3 board — I2C, NAU8822 codec, 4-bit SD, ST7789 or ILI9488
display. `pins`, `audio`, `i2c`, `sd`, `display`.

**The display connector takes either a 240×280 ST7789 or a 320×480 ILI9488**,
on the same pins. Only the ST7789 preset exists so far; `board-minstro display`
drives that one. Fit the ILI9488 instead and it needs its own preset once that
driver exists — see [display.md](display.md#adding-a-panel).

### board-core-basic

M5Stack Core Basic, ESP32. `pins`, `audio`, `mic`, `i2c`, `sd`.

**GPIO 13 is shared between SD CS and speaker DOUT.** That is a board-level
design decision, not something the firmware can work around: only one subsystem
can have the pin at a time, so running `board-core-basic sd` and then
`board-core-basic audio` (or the reverse) reconfigures it for the new purpose.

### board-sinn

Sinn, ESP32-S3FH4R2 — 4-bit SD with card detect, I2C with an SHT4x, optional NAU7802, eight PDM
microphones, Vin sense. `pins`, `sd`, `i2c`, `nau7802`, `mic`, `mic-array`, `vin`.

**Build with the quad-PSRAM overlay.** The default S3 build enables octal PSRAM,
which claims GPIO 33–37 (the I2C bus and DRDY here) and does not boot on this chip:

```
idf.py -B build_sinn -DSDKCONFIG=sdkconfig.sinn \
  "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.sinn" build
```

- `sd` runs `sd mmc 42 41 43 44 39 40`, 4-bit. It leaves out `cd 38`: with a card seated
  GPIO 38 floats rather than going low, and the driver reads a high CD pin as an empty
  slot, so `cd 38` makes the card undiscoverable. Measured on a 3.68 GiB SDHC card:
  verified reads up to 40 MHz (the S3's limit), 12 MiB/s.
- `i2c` runs `i2c bus 35 36`. The SHT4x temperature/humidity sensor is at 0x44, read with
  `i2c-sht4x read 0x44`. `i2c scan` also lists INA2xx and PI4IOE5V6408 parts at 0x44,
  because they share the address; on this board it is the SHT4x.
- `nau7802` runs `i2c bus 35 36` then `i2c-nau7802 init drdy 37`. It is separate from
  `i2c` because the part is optional.
- `mic` opens the first microphone pair (`audio pdm 10 11`), decoded to PCM, so
  `audio level` and `audio record` work. `mic-array` opens all eight as raw bits
  (`audio pdmraw 10 11,12,13,14`), for `audio pdmbits`.
- `vin` runs `gpio aread 1`. The divider is 500k over 100k, so the input voltage is
  the reported millivolts × 6 (about 18.6 V at full scale).

The pins are from the board designer and have not yet been exercised here.

## Adding a board

A `board_t` entry in `main/board/board.c` — name, chip, description, the pin
table, and a preset per subsystem as the command lines a person would have typed
— plus a group in `main/app_console.c`. The board's name is its group name minus
the `board-` prefix, so it has to be spelled like a group name: `core-basic`, not
`core_basic`.
