# Touch

[← Command reference](README.md) · [Project README](../README.md)

The ESP32-S3 has a capacitive touch peripheral on GPIO1–14, one channel per
pin. `<pads>` accepts the same `4`, `0-5` or `1,4,8-10` forms as every `gpio`
command, and the numbers are GPIO numbers — the channel a pad uses is the same
number, so there is nothing extra to look up.

**Not available on the ESP32-C3** — this project's default build target has no
touch sensor peripheral at all. The command still appears in `help`, but
refuses with a suggestion to `idf.py set-target esp32s3`.

## `watch <pads> [seconds]`

Calibrates a baseline for every pad given, then reports **all of them, every
tick**, for the requested duration (default 15 s, up to 120 s).

Reporting every pad on every line rather than just the one that crossed a
threshold is the point: the question a touch pad array bring-up actually has
is not "does this pad work" but "does pressing this pad also move its
neighbour" — crosstalk between adjacent pads is invisible if the tool only
tells you about the pad that triggered.

Calibration runs first, with nothing touched: each pad's smooth reading is
averaged for about a second to get a baseline, then sampled again to find its
peak deviation from that baseline ("noise"). A touch is reported once a pad's
deflection clears six times its own calibration noise — a per-pad guard, not
a single number across the whole board, since pad size and routing affect how
noisy a channel's idle reading is.

```
> touch watch 1,2,3
Calibrating 3 pads -- do not touch any of them...
Baseline:  GPIO1    412 (+/-3)  GPIO2    408 (+/-4)  GPIO3    397 (+/-2)

Watching for 15 s. Touch any pad -- all pads are reported every tick, so a
neighbour reacting is as visible as the one you touched.
   0.0s  GPIO1    413 (    +1)        GPIO2    409 (    +1)        GPIO3    398 (    +1)
   0.2s  GPIO1    411 (    -1)        GPIO2    407 (    -1)        GPIO3    396 (    -1)
   0.4s  GPIO1    892 (  +480) TOUCH  GPIO2    431 (   +23)        GPIO3    399 (    +2)
   0.6s  GPIO1    901 (  +489) TOUCH  GPIO2    429 (   +21)        GPIO3    398 (    +1)
   0.8s  GPIO1    414 (    +2)        GPIO2    410 (    +2)        GPIO3    397 (    +0)
...
Peak deflection from baseline over the run:
  GPIO1   peak   489  (163.0x calibration noise)
  GPIO2   peak    23  (5.8x calibration noise)
  GPIO3   peak     2  (1.0x calibration noise)
```

GPIO1 was pressed. GPIO2's reading moved too — 23 counts, under this pad's
6x-noise significance margin so it is not flagged `TOUCH`, but visible in both
the live line and the peak summary. That is the crosstalk this command exists
to catch; whether 5.8x calibration noise on a neighbour is fine for a given
board is a judgement call the pad layout has to answer, not this tool — the
number is what makes that judgement possible.

Pins outside GPIO1–14, or repeated in the list, are skipped with the reason
rather than silently reducing the set under test.

Nothing persists between runs: the touch controller and every channel are
released when `watch` finishes (or fails partway through), so a re-run
recalibrates from scratch. Compare against [`gpio rc`](gpio.md#rc-pin-ref-pin-kohms)
if a pad reads active or inactive with unusual confidence — a pull-up that
should not be there, or a net that is not connected at all, both look plausible
until compared against a schematic value.
