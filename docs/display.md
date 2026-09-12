# Display

[← Command reference](README.md) · [Project README](../README.md)

Solid colours, a grid over the whole display area, and an orientation/offset
tool, over an SPI panel.

This is a capability menu rather than a bus menu — it owns SPI the way `sd`
owns whichever bus a card is on. The generic commands here never mention a
specific controller: `display fill`, `display bars`, `display grid` and
`display edges` work through a small panel interface, so an ST7789 and a
future ILI9488 are driven by the same patterns. A panel appears as its own
submenu (`display-st7789`) and attaches itself when initialized.

**There is no framebuffer.** A 480×320 frame at 3 bytes per pixel is 460 kB,
and none of the chips this targets have PSRAM. Every pattern is drawn a band of
rows at a time into a small DMA buffer.

**SPI3, not SPI2, on chips that have it.** SPI2 is shared by the `spi` and `sd`
groups; putting the display on SPI3 lets a display and an SD card run at the
same time on the ESP32 and S3. The ESP32-C3 has only SPI2, so there `display`
joins `spi` and `sd` in refusing rather than stealing the host — see
[the three-way note in spi.md](spi.md) and [sd.md](sd.md).

## `bus <clk> <mosi> <cs> <dc> [rst <pin>] [bl <pin>] [miso <pin>] [khz <n>]`

Opens the SPI host and the panel IO. No panel is attached yet — that is a
separate command, per panel, below. Re-running it closes any previous bus and
panel first, so it can be re-run with new pins.

The clock defaults to 20000 kHz; lower it with `khz` to rule out wiring or
signal integrity before suspecting the panel. `miso` is normally omitted — most
of these panels are write-only over their connector — and `rst`/`bl` are
optional GPIOs for the panel's reset and backlight.

## `panels`

Lists the panels this firmware can drive, and marks the one attached, like
`audio codecs`.

## `info`

Host, pins, clock, and — once a panel is attached — its name, dimensions, gap,
orientation, colour order and inversion. Ends by printing the `bus` and
`display-<part> init` lines that reproduce the current state, ready to copy
into a board preset.

## `fill <red|green|blue|white|black|0xRRGGBB>`

Fills the screen with a solid colour.

## `bars`

Eight vertical bars: white, yellow, cyan, green, magenta, red, blue, black. Red
and blue swapped means the panel needs `bgr`; the white and black ends swapped
means flip `invert`.

## `grid [step]`

A 1 px line every `step` pixels (default 20), plus the border. Shows dead rows
or columns, scaling, and edge clipping.

## `edges`

**The orientation and offset tool.** A 1 px border with a different colour per
side — top red, right green, bottom blue, left white — a filled 8×8 yellow
square at logical (0,0), and a short cyan tick along +X next to it. A missing
coloured side means the gap on that axis is off; noise on one side means the
gap is too large.

## `gap <x> <y>`

Sets the panel's gap and redraws `edges`.

## `orient <swap_xy> <mirror_x> <mirror_y>`

Each argument is 0 or 1. Swapping also swaps the logical width and height.
Redraws `edges`.

| swap_xy | mirror_x | mirror_y | Rotation |
|---|---|---|---|
| 0 | 0 | 0 | Native (0°) |
| 1 | 0 | 1 | 90° |
| 0 | 1 | 1 | 180° |
| 1 | 1 | 0 | 270° |

## `invert <on|off>`

Toggles `esp_lcd_panel_invert_color()` and redraws `bars`.

## `backlight <on|off>`

Drives the `bl` GPIO given to `bus`, if there is one. See
[`gpio-pwm set`](gpio.md#set-pin-freq-duty) for dimming rather than an on/off
switch.

## `close`

Deletes the panel and the panel IO, frees the SPI host, and resets the pins.

## ST7789

Reached as `display-st7789 ...`. Drives the ST7789 SPI TFT controller —
240×320 controller RAM, RGB565 over SPI. The ST7789V2 on the M5Stack
Cardputer is the same controller under a different marketing name; what
differs board to board is glass size, gap, inversion and colour order, which
is exactly what `bars`, `edges` and `gap` exist to measure.

### `init <width> <height> [gap <x> <y>] [bgr] [invert on|off]`

Requires `display bus` first. Attaches the panel, turns the backlight on, and
draws `edges`, so a successful init is visible immediately.

`invert` defaults to on: IPS glass usually needs `INVON` for correct colour,
and the default is what most of these panels want out of the box. Give
`invert off` if colours look inverted with the default.

## Adding a panel

A new `panel_*.c`/`.h` exporting a `display_panel_t` (see `main/display/display.h`),
a row in `panel_registry[]` in `main/display/display.c`, and a group in
`main/app_console.c`. The generic commands above need nothing added — they
work through `bytes_per_pixel`, `pack()` and `create()`, the same way
`audio tone` works with any codec.
