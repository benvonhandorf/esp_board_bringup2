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

## Adding a board

A `board_t` entry in `main/board/board.c` — name, chip, description, the pin
table, and a preset per subsystem as the command lines a person would have typed
— plus a group in `main/app_console.c`. The board's name is its group name minus
the `board-` prefix, so it has to be spelled like a group name: `core-basic`, not
`core_basic`.
