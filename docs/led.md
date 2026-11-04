# LED

[← Command reference](README.md) · [Project README](../README.md)

Addressable LED chains. Today that is the **WS2812** family (also SK6812 RGB
and WS2811 at 800 kHz): one data pin, driven by the RMT peripheral. The driver
sits behind a small interface (`main/led/led_drv.h`) so a two-wire type such as
APA102 can be added without touching the commands.

The chain is sent from a dedicated task, so the console stays usable while
`cycle` runs. Colour order on the wire is handled by the driver; every command
here takes and shows **R, G, B**.

Full-brightness LEDs draw up to about 60 mA each. Mind the supply on a long
chain, and share ground between the board and the strip.

## `config <type> <count> <data_pin>`

Declares the chain: `type` is `ws2812`, `count` is 1–1024, and `data_pin` any
output-capable GPIO. Every other command needs this first. Running it again
reconfigures (stopping any cycle and releasing the old pin); the strip is then
set to all off.

## `fill <r> <g> <b>` / `fill <0xRRGGBB>`

Sets every LED to one colour: three values 0–255, or one packed hex value
(`fill 0xff00ff` is `fill 255 0 255`). Ends a running cycle. `fill 0 0 0` turns
the chain off. The hex form is the same R,G,B as the three-value form; no
allowance is made for LED types that wire in a different colour order.

## `rainbow`

Shows one static rainbow spread across the whole chain. Ends a running cycle.

## `cycle [seconds_per_cycle]`

Rotates the rainbow continuously, one full trip round the colour wheel every
`seconds_per_cycle` (0.5–3600, default 5). Returns immediately; the task keeps
sending at 50 frames a second until `stop`. Running it again changes the speed.

## `stop`

Ends a running cycle, leaving the last frame displayed.
