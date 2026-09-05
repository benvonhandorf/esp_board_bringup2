# GPIO

[← Command reference](README.md) · [Project README](../README.md)

Commands in this menu take either a single pin number, a range of pins (e.g. `0-5`) or a comma separated list of pins as a <pin> parameter.  

## `set <pin> <state>`

Sets the specified pin(s) to the requested state.  If pin is not already configured as an output pin, it will be configured at this time.

<state> options:
- true, 1, high: pin will be set high
- false, 0, low: pin will be set low

## `read <pin> [up|down|none]`

Returns a list of the specified pins and their logic levels.  If pin is not already configured as an input pin, it will be configured at this time.
Output will be one pin number per row, followed by a 0 or 1 indicating the logic level of the pin.

The optional internal pull defaults to `none`, which is what a bare `read` has
always done. `up` is the useful one during bringup: without a pull, a pin that is
driven low and a pin that is connected to nothing both tend to read 0. With the
pull-up fighting it, anything still reading 0 is genuinely being held down.

Sample: `gpio read 1,4` when GPIO 1 is low and GPIO 4 is high.
Output:
```
1: 0
4: 1
```

## `aread <pin>`

Reads the analog value from the specified pin and returns a digital representation.
Both the raw ADC count and, where the chip carries the necessary calibration data
in eFuse, the equivalent voltage in millivolts are reported.

## `blink <pin> <count> [period_ms]`

Blinks the specified pin LED count times for visual testing. The period defaults
to 500 ms. Accepts pin lists like the other GPIO commands.

## `short <pin>`

Finds pins that are shorted together. Give it the pins to test — the same
`4`, `0-5` or `1,4,8-10` forms as every other GPIO command — and it drives each
one low in turn while reading the others with pull-ups enabled. A pin that
follows another down shares a net with it.

**Adjacent pins are tested and reported first.** A solder bridge at the package
is far and away the most common way two nets get tied together, and neighbouring
GPIO numbers are usually neighbouring pads, so that is the answer worth seeing
before a wall of results. (Usually, not always — GPIO numbering does not track
the package outline exactly, so the full matrix follows.)

A short is only reported when **both** pins pull each other down. The symmetry
requirement matters: a pin held low by the board — a grounded net, a card-detect
switch, an output driving low — follows everything and fights nothing, and a
naive rule reports it as shorted to every pin in the list. Those are listed
separately as untestable instead.

Pins that must never be driven are skipped with the reason given: flash and PSRAM
pins, the USB or UART console pins, and anything a peripheral currently holds.
The first group would hang the chip and the second would cut off the connection
these results are printed to; ESP-IDF's own `esp_gpio_is_reserved()` supplies the
list, so it stays right per chip and per board configuration.

Because it drives every pin listed, do not run it against a bus another device
may be driving at the same time. Pins are left as passive inputs afterwards.

Sample, on a board where D0 and SCK of an SD slot were bridged at the pads:

```
> gpio short 38-44
Adjacent pins (where a solder bridge usually lands):
  GPIO 39 <-> GPIO 40   SHORTED
Remaining pairs:
  15 pairs tested, all clear
Held low whatever is driven, so not testable and not a short:
  GPIO 44
```

That short stalled SD card init in a way that looked nothing like a wiring
fault — the host never got its clock started, because it waits for the data bus
to go idle and every CLK low was dragging D0 down with it.

## `survey [pin]`

**The first command to run on a board you do not know.** It measures every pin
the chip can safely drive, classifies each as pulled up, driven, or floating,
and says what that suggests. On an unfamiliar carrier it found the I2C bus
immediately:

```
> gpio survey
Surveying 45 pins. Nothing else may be driving them.

 pin  state             pull-up      net C
   6  pull-up           2.2 k>         --  too strong to measure
   7  pull-up           2.2 k>         --  too strong to measure
  10  driven low             --         --  something is holding this pin
  19  skipped        USB console
  27  skipped        reserved for flash, PSRAM or a peripheral in use
  38  weak pull-up     61.4 k      7.6 pF
  46  floating             none     2.6 pF
...
GPIO 6 and 7 are the only pins with a deliberate pull-up, which is what an
I2C bus looks like. Try:
    i2c bus 6 7   (then 'i2c scan')
    i2c bus 7 6   (SCL and SDA the other way round)
```

**It refuses to drive what it must not.** Flash, PSRAM and any peripheral
currently holding a pin are excluded through ESP-IDF's own reservations; the
console pins are excluded by name, because sweeping the USB or UART pins cuts
off the connection the results are printed to. Each skipped row says which.

An I2C bus is the one thing this can genuinely infer — nothing else routinely
puts a few kiloohms across a pair of pins. **Capacitance is not a second
inference.** It separates a bare pad from a routed net and nothing more: a
clock or data line is *driven*, not pulled, and from inside the chip it looks
like any other idle input. That is stated in the command's own output because
the temptation to read more into it is strong and costs real time — a
permutation search built on exactly that assumption failed on a board whose
I2S lines were sitting among the unremarkable-looking pins.

## `rc <pin> [ref <pin> <kohms>]`

The single-pin version, when you want the numbers rather than the verdict, or
want to calibrate against a known resistor. Measures the **pull-up strength and
capacitance of each net**. `short` answers
"are these two nets the same net"; this answers "is this net pulled up, how
hard, and how much is hanging off it" — which a logic read cannot, because a
10k pull-up to a healthy rail and a 10k pull-up whose far end is floating both
read as `1`.

It drives the pin low, releases it, and times the rise. The net charges through
whatever pulls it up, so the rise is R×C. Measuring twice, the second time with
the internal pull-up (~45k) added in parallel, gives two equations for the two
unknowns and uses the on-chip pull-up as the reference:

```
t_ext = k·R_ext·C        t_par = k·(R_ext ‖ R_int)·C
t_ext / t_par = (R_ext + R_int) / R_int
```

so `R_ext = R_int · (t_ext/t_par − 1)`, and `C` follows. No meter needed.

**The internal pull-up is the one thing that cannot be measured from the
inside**, and every result scales with it. Nominally 45k, it varies widely with
process and temperature — on the ESP32-S3 measured here it is actually 35k. So
uncalibrated results are worth about a factor of two, which is still enough to
separate the cases this exists for: a signal net is single-digit pF, a net tied
to an unpowered rail's decoupling capacitor is tens of nF.

**Ratios between pins are exact regardless**, because the threshold and the
capacitance cancel in `t_ext/t_par`. Two pins that differ by 2× really do differ
by 2×, even when neither absolute value can be trusted. Read the table that way
before reading the numbers.

To get absolute accuracy, name a pin whose pull-up you know and the measurement
runs backwards to solve for `R_int`, which then applies to every other pin:

```
> gpio rc 38-44 ref 41 50
Internal pull-up measured as 34.9k against the 50 reference on GPIO 41.
```

A good reference is a net with a known fitted resistor and nothing else on it.

Timing is done by releasing the pad and sampling it once at a chosen delay,
binary-searching for the crossing, with the delay-to-sample cost calibrated out
by driving the same pin push-pull. Polling `gpio_get_level()` in a loop instead
only resolves one iteration — about 290 ns here, the same order as the rise
being measured, which quantises every net to the same two numbers.

Sample, on the SD slot of an ESP32-S3 board with 50k pull-ups fitted on CMD
and DAT0–3, calibrated against one of them, with a card in the slot:

```
> gpio rc 38-44 ref 41 50
Internal pull-up measured as 34.9k against the 50 reference on GPIO 41.

 pin    external   with int    pull-up      net C
  38      650 ns      250 ns    55.9 k      8.4 pF     D1
  39      650 ns      269 ns    49.5 k      9.5 pF     D0
  40        none      462 ns       none      9.6 pF    CLK
  41      650 ns      275 ns    47.6 k      9.8 pF     CMD
  42      369 ns      200 ns    29.5 k      9.0 pF     D3
  43      650 ns      238 ns    60.7 k      7.7 pF     D2
  44          --         --         --         --      DET
```

Reading that: **CLK correctly has no pull-up** — the host always drives it
push-pull, and a pull-up there would be a design error. **DET is held low** by
the card-detect switch, which is how you know a card is seated. CMD and DAT0–2
average 53k against a fitted 50k, so the scatter is the measurement, not the
board.

**DAT3 at 29.5k is the interesting one, and it is correct.** Solving
`50k ‖ X = 29.5k` gives 72k, which is the pull-up *inside the card* used for
card detection before the host knows anything about the card — the SD spec puts
it at 10–90k. Every SD slot with a card in it should show DAT3 at roughly half
its neighbours. Pull the card and it rises to match them.

Every net is 8–10 pF: a bare pin and a short trace, nothing unexpected hanging
off the bus.

Holding the other lines low with `gpio set` and re-measuring one of them is a
way to ask whether a rail is real: if a net's pull-up is being fed parasitically
through its neighbours, collapsing them changes the answer. If it does not
budge, the rail is genuinely sourced.

Same cautions as `short`: it drives the pin, so nothing else may be driving it,
and interrupts are masked for up to 5 ms per sample.

## PWM

Reached as `gpio-pwm ...`, or by entering the `gpio` menu and then `pwm`.
Backed by LEDC; channels and timers are pooled, and outputs at the same
frequency share a timer.

### `set <pin> <freq> <duty>`

Configures PWM on the specified pin with the given frequency (in Hz) and duty cycle (0-100%).
Re-running it on the same pin retunes that output in place. The duty resolution
is chosen to be the finest the clock allows at the requested frequency, and the
frequency actually achieved is reported (the timer divider quantizes it).

### `stop <pin>`

Stops PWM output on the specified pin, leaving it low, and releases the channel.
