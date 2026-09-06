# Loadcell

[← Command reference](README.md) · [Project README](../README.md)

Drives an Avia Semiconductor **HX711** as a load cell front end. It is a
top-level menu rather than a submenu of a bus, because the HX711 has no bus:
two pins carry everything, and this driver bit-bangs both.

Verified against a real HX711 on 2026-08-26 — see
[HardwareSupport.md](../HardwareSupport.md) for exactly what the run reached
and what it did not. The few constants the datasheet does not actually state
are marked `UNVERIFIED` in `components/hx711/src/`.

The normal bringup sequence is:

```
loadcell init 5 6           # DOUT on GPIO 5, PD_SCK on GPIO 6
loadcell gain 128           # the default, and what a load cell normally wants
loadcell tare               # with the scale empty
loadcell calibrate 100      # with a known 100-unit mass on it
loadcell weight             # thereafter, in those units
```

It presents very nearly the same commands as
[`i2c-nau7802`](i2c.md#nau7802), deliberately — the two parts do the same job,
and the arithmetic from `tare` onwards is identical. Everything below the
commands is different.

## How the part works, and what that costs

There are no registers. The device pulls **DOUT** low when a conversion is
ready; the host clocks 24 bits out of it on **PD_SCK**, and then keeps
clocking, because the *number* of trailing pulses is how the next conversion's
channel and gain are chosen:

| Pulses | Channel | Gain |
|---|---|---|
| 25 | A | 128 |
| 26 | B | 32 |
| 27 | A | 64 |

Those three are the whole configuration surface. **Gain and channel are one
setting, not two** — channel B's gain is fixed at 32 and channel A cannot be
set to it — which is why [`gain`](#gain-3264128) and [`input`](#input-ab) both
print the pair they landed on rather than the field you asked for.

Two consequences worth knowing before the numbers stop making sense:

**A change costs conversions.** The trailing pulses configure the *next*
conversion, and Table 2 then allows 400 ms at 10 SPS or 50 ms at 80 SPS to
settle — four output periods either way, which is the happy accident that lets
this driver settle correctly without knowing which rate the board is strapped
to. Every setting change discards five conversions and says so. A change that
skipped them would report the previous gain once, off by exactly a factor of
two, and self-consistent.

**A change also voids the calibration.** The tare and scale were captured at
the old gain and are wrong by the same factor, so they are dropped and said to
be dropped. Losing a calibration loudly beats keeping one that quietly reports
the wrong weight.

## The sample rate is a strap, so there is no `rate` command

The HX711's output rate is set by pin 15: tied low it converts at 10 SPS, tied
high at 80. Nothing in the serial protocol can change it. Generic breakouts tie
it to ground on the PCB with no jumper at all; SparkFun's has a jumper you cut.

So instead of setting the rate, [`status`](#status) **measures** it — timing a
short run of real conversions and reporting which strap that implies. That is
worth having for its own sake: the internal oscillator gives 10 or 80 and
nothing between, so a measurement that lands in between is itself a diagnosis,
usually that XI (pin 14) is not grounded and the part is running from a crystal.

## `init <dout> <sck> [gain <32|64|128>] [scale <counts_per_unit>]`

Claims the two pins, resets the part, applies the gain and proves something is
actually there.

`scale` installs a factor from an earlier bench calibration instead of measuring
one — see [below](#scale-counts_per_unit). It belongs on `init` rather than
being set afterwards, because `init` power-cycles the part and clears the tare
and scale on the way through, so a factor set before it would be silently gone.
Giving it here also ties it to the `gain` in the same line, which is the only
way the two are ever correct together. It is applied last, after the part has
answered a real batch of conversions — every failure before that point releases
the pins, and a factor left sitting on a disowned part would be worse than none.

`<dout>` is the GPIO on the part's DOUT — an input here — and `<sck>` the one
on PD_SCK, an output. PD_SCK is checked against the same list `gpio` uses
before it drives anything: input-only pins, flash and PSRAM pins, and **the
console's own pins** are refused, the last because driving one would end the
session that typed the command.

**The liveness check is in two halves, and needs both.** A part with no ID
register cannot be asked to identify itself, so `init` proves each wire
separately:

- **DOUT falls.** The pin is configured with a pull-up, so a floating or
  mis-numbered pin sits high and times out. A pin shorted to ground never read
  high to begin with. Only something driving the line low can produce the
  transition.
- **DOUT comes back high after the burst.** The datasheet is explicit that the
  25th PD_SCK pulse is what pulls DOUT high again, and nothing else does inside
  a conversion period. So this is a positive test that the clock physically
  reached the device — the half of the wiring that receiving data cannot prove
  on its own.

It then takes five conversions and refuses if they are bit-identical. A 24-bit
sigma-delta on a live bridge always dithers by a few counts; an exactly
repeating value is what a DOUT pin not carrying data looks like, and all-zeros
in particular reads as a perfectly plausible empty scale.

**Why the pull-up, when the NAU7802 pulls DRDY down.** Same rule, opposite
polarity. The rule is to bias the ready line toward *nothing available*, so an
absent device produces a timeout instead of a number. NAU7802's DRDY is active
high, so it is pulled down. The HX711's DOUT is active **low**, so it is pulled
up. A pull-down here would be the single worst choice available in this driver:
an unconnected pin would read ready forever, clock 24 zeros out of nothing, and
report a clean `0` — and `tare` would happily capture it.

## `status`

Splits into what is true about the part and what this session set.

`i2c-nau7802 status` reads its answers back out of silicon, which is what makes
it authoritative. That is not available here: no bus, no registers, and without
`init` not even a pin to read them on. So `status` says which half is which
rather than presenting firmware state as hardware fact. Without `init` it still
prints the part's own constants — the three legal settings, the coding, the
input range, the common-mode window — and then says it is not initialized.

A real run, on a bridge with no load, at gain 128:

```
> loadcell init 10 3
Waiting for the first conversion...
Settling: discarding 5 conversions
HX711 responding on DOUT GPIO 10, PD_SCK GPIO 3
Channel A, gain 128 (25 clock pulses)
Rate 11.6 SPS -- RATE strap low (10 SPS nominal), period 86 ms
Idle reading -103504.8 counts (5 samples, spread 76)
Run 'loadcell tare' with no load, then 'loadcell calibrate <known mass>'
```

With a factor from an earlier bench run, `init` reports it and points at the one
measurement still outstanding:

```
> loadcell init 10 3 gain 128 scale 1074.3
...
Idle reading -103504.8 counts (5 samples, spread 76)
Scale 1074.3 counts per unit (supplied, not measured)
Run 'loadcell tare' with no load, then 'loadcell weight'
```

The input range line is worth reading. It is ±0.5 × AVDD/gain, so it scales
with the supply, and a great many breakouts are run at 3.3 V off the ESP rather
than the 5 V the datasheet's front page assumes. The familiar "±20 mV at gain
128" is ±12.9 mV at 3.3 V, and a cell sized for the first saturates a third
early on the second.

The common-mode window (AGND+1.2 V to AVDD-1.3 V) is printed for the opposite
reason: a bridge sitting outside it produces confident, wrong numbers, and no
amount of firmware can see that. It is there so you can check it with a meter.

`status` also reports the worst PD_SCK high phase seen since `init`, and how
many bursts exceeded the limit and were discarded — see below.

## `gain [32|64|128]` and `input [a|b]`

Two views of one setting. `gain 32` selects channel B, because that is the only
gain it has; `input a` returns to whichever of 64 and 128 was last in use,
since naming the channel does not say which. Both print the full resulting
state, and both discard the settling conversions.

`input b` warns every time. The datasheet's own application example uses channel
B for battery monitoring, and on a load cell breakout INB+/INB− usually go
nowhere — so it reads a floating input, which drifts convincingly rather than
failing.

## `power [on|off]`

`power off` holds PD_SCK high, which is how the part powers down: current drops
from about 1.5 mA to 0.5 µA, and if the internal regulator feeds the bridge,
that goes down too.

It exists because the power-down entry is also this driver's main hazard.
PD_SCK high for more than 60 µs is not a long clock pulse, it is a reset — and
coming back up reverts the channel and gain to A/128. Giving that a name makes
it a tool instead of only a trap; `power on` re-applies the configured setting
and says that it had to.

**The tare and scale survive a power cycle**, which is the one path that
preserves them across a reset of the *part*. `power on` re-applies the mode this
driver already holds rather than rewinding to the part's A/128 default, so the
gain comes back identical and the factor captured at it still applies — the
setting has not changed, so nothing is dropped. A `gain` or `input` change is
the opposite case and does drop them.

## `raw [samples]`, `read [samples]`, `tare [samples]`, `calibrate <known mass> [samples]`, `weight [samples]`

Identical in behaviour to their [`i2c-nau7802`](i2c.md#nau7802) counterparts,
including the guard that makes `calibrate` refuse when the reading has not
moved clear of the noise — see
[the note there](i2c.md#read-samples-tare-samples-calibrate-known-mass-samples-weight-samples)
for why that guard tests the uncertainty of the averages rather than the spread
of the samples. It has never tripped on an HX711: at ~48 counts RMS this part
is about a hundred times quieter than the NAU7802 on the same board, so a
10-sample tare pins its mean to ±10 counts and any real mass clears the
threshold by orders of magnitude.

`raw` takes no `[a|b]` channel argument, unlike the NAU7802's. Switching
channel costs five conversions of settling — half a second at 10 SPS — so
alternating per sample would spend nearly all its time settling.

`read` warns before a batch that will take more than a couple of seconds. The
200-sample maximum is inherited from the NAU7802, where it is under a second;
on an HX711 strapped to 10 SPS it is twenty, and a console that prints nothing
for twenty seconds looks like a hang worth power-cycling.

**The tare and scale live only in this firmware's memory.** A reset of the ESP
loses them. That is the same deliberate split the NAU7802 has, with one fewer
consolation: there, the registers survive in silicon and `status` can still
show you a configured, converting part. Here there is nothing to survive.

Only half of that has to be lost, though, and the asymmetry is the point of
[`scale`](#scale-counts_per_unit). A scale factor is a property of the cell and
the gain, so it can be measured once and handed back on every boot. The tare is
a measurement of where zero currently is, and it has to be repeated.

## `scale [counts_per_unit]`

With no argument, reports the factor in force and where it came from. With one,
sets it — without measuring anything. Identical in behaviour to
[`i2c-nau7802 scale`](i2c.md#scale-counts_per_unit), including the `%.17g`
pasteable line, the refusal of a zero factor, the acceptance of a negative one,
and `weight` refusing when a factor is set but no tare has been taken.

```
> loadcell calibrate 100 50
Calibrated: 1074.3 counts per unit (107430.0 counts for 100.0000 units)
Scale good to +/-0.05% (53.7 counts of uncertainty)
    scale factor for the consumer:  1074.3  /* +/-0.05%, channel A gain 128, counts per unit */

> loadcell init 10 3 gain 128 scale 1074.3
...
Scale 1074.3 counts per unit (supplied, not measured)
Run 'loadcell tare' with no load, then 'loadcell weight'
```

Gain and channel are one setting on this part, so `gain` and `input` both drop a
supplied factor exactly as they drop a measured one — a factor is counts per
unit at one gain, and at another it is wrong by exactly the gain ratio. The
message says which kind was dropped, because the fix differs: a measured factor
is re-measured here, a supplied one has to come from a bench run at the new
gain.

## `close`

Releases both pins to their reset state, leaving PD_SCK **low** on the way out.
A clock pin left high would power the part down sixty microseconds later, and
the next session would find a device that had silently reset.

## Timing, and the one failure that would not show

The 25–27 clock pulses run inside a critical section, with interrupts masked
for about 60 µs. That is bounded and stated in the source; for comparison
[`gpio rc`](gpio.md#rc-pin-ref-pin-kohms) already accepts a 5 ms bound for its
own measurement.

The burst is also placed in IRAM, and that is load-bearing rather than
decoration. Masking interrupts stops a task or an ISR from preempting the
clock, but code running from flash can still stall on an instruction-cache
miss, and a flash write elsewhere in the system disables the cache outright for
milliseconds. Either would stretch whichever high phase it landed in past 60 µs
and power the device down **mid-transfer** — which does not produce a late
reading or an obviously broken one. It produces a believable number, at a gain
nobody asked for, with the tare and scale still applied.

So each high phase is timed individually and the reading is discarded if any of
them exceeded the datasheet's 50 µs limit. Individually, not as a total: the
*low* phase has no maximum at all, so a burst can come in at a perfectly normal
total duration while one high phase inside it blew the limit.

Between 50 µs and 60 µs the datasheet says nothing — the first is where the
timing spec ends, the second is where power-down is documented to begin. This
driver treats anything past 50 µs as a failure rather than assuming the gap is
safe.

## The gain is a test instrument

With nothing plugged in changing, the reading must track the gain setting — and
on this part that is the only available proof that the trailing pulse count
does what the datasheet says. Measured on the bench:

| Setting | Pulses | Reading (20 samples) |
|---|---|---|
| A / 128 | 25 | −103,570 |
| A / 64 | 27 | −54,336 |
| B / 32 | 26 | −11,156 |

128/64 comes out at **1.91** against an ideal 2.00. The shortfall is not error:
solving the two readings for a gain-independent term gives about −5100 counts
of offset that does not scale, which is exactly what the model predicts. The
number that matters is that the ratio is *near two and the right way up* — 25
pulses reads roughly double 27. Had the mapping been inverted, or had Figure
2's mislabelling been taken at face value, this would have come out at 0.5.

Channel B does not fit the same line, and should not: it is a different
physical input, usually connected to nothing.

## Why this part gets a driver

The [bringup guide](bringup.md) sets the test: a part earns a driver when it
can **fail silently**. Every trap above produces a plausible number rather than
an error — the coupled gain and channel, the stale reading after a change, the
power-cycle inside a clock burst, and a disconnected DOUT reading as an empty
scale. None of them is visible in the output they produce, which is exactly the
case a bringup tool exists to catch.
