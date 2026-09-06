# I2C

[← Command reference](README.md) · [Project README](../README.md)

## `bus <scl> <sda>`

Initializes an I2C bus for use by future commands, specified by the `scl` and `sda` pins.

## `scan`

Scans the I2C bus enumerates the found devices in a table with the hexadecimal least significant digits along each column and the most significant digits as rows.  If a device is not found, the intersection should be left blank.  If one is found, print the address in hex at the intersection.

If any errors are present that prevent the bus from being properly scanned, such as missing pull up resistors, print the state of the bus that prevents scanning.

Below the table, every address that answered is listed again with the parts
this firmware can drive there and the command that would drive each one:

```
3 devices found

ADDRESS   PART          WHAT                                      TRY
0x2a      NAU7802       24-bit bridge ADC, load cell front end    i2c-nau7802 init
0x44      PI4IOE5V6408  8-bit I/O expander, 5 V tolerant          i2c-pi4ioe init 0x44
0x44      SHT4x         humidity/temperature; A/B/C = 0x44/45/46  i2c-sht4x read 0x44
0x44      INA219        current/voltage/power monitor             i2c-ina219 read 0x44
0x44      INA226        current/voltage/power monitor             i2c-ina226 read 0x44
0x44      INA237        current/voltage/power monitor             i2c-ina237 read 0x44
0x46      SHT4x         humidity/temperature; A/B/C = 0x44/45/46  i2c-sht4x read 0x46
```

**This is a suggestion, not an identification.** Nothing about an address
distinguishes one part from another, and the overlaps are large — the three
current monitors share the whole of 0x40–0x4f, and 0x44 is an SHT4x-A, a
PI4IOE5V6408 with ADDR high, or any of them. So every candidate is listed, and
the command beside it is what settles the question: each driver reads the
part's own ID and refuses an address that answers with the wrong one. Pointing
all three monitor groups at 0x46 and watching two refuse is exactly how the
part on the sensor board was identified.

Candidates are ordered tightest address range first, because that is the only
ranking available without touching the bus: a part with two possible addresses
is a stronger guess at one of them than a part with sixteen.

Only parts with a driver here are named. An address no group claims gets a row
saying so, and `i2c read` as the one thing left to try:

```
0x68      -             no driver in this firmware claims it      i2c read 0x68
```

## `read <address> <bytes=1>`

Reads the specified number of bytes from the specified address on the bus.  If the number of bytes are not specified, one byte is read.

## `identify [address]`

The same lookup as the table under `scan`, for one address, or — with no
address — the whole catalogue of parts this firmware can drive over I2C, with
the range each one occupies.

This does not touch the bus and does not need `i2c bus` to have been run: it
reads a table compiled into the firmware, so it answers while the wiring is
still being decided. Whether anything is actually *at* the address is what
`scan` is for.

## INA237

Reached as `i2c-ina237 ...`, or by entering the `i2c` menu and then `ina237`.
Drives TI INA237 current/voltage/power monitors, which occupy addresses
`0x40`–`0x4f` depending on how their A0/A1 pins are strapped. Several can be
present on one bus, each with its own shunt resistor.

The shunt input range is left at the device default of ±163.84 mV. Combined
with the shunt resistance this fixes the current resolution: at the default
0.004 Ω the device reads ±40.96 A at 1.25 mA per count.

### `config <address> [shunt_ohms]`

Registers a monitor and programs its calibration. The shunt resistance defaults
to **0.004 Ω**. Before accepting the device, `MANUFACTURER_ID` is read and must
report `0x5449`, so a wrong address or a different chip is reported rather than
silently producing plausible-looking numbers. Re-running on the same address
updates it in place.

Note that `MANUFACTURER_ID` is the only identification available: unlike the
INA238 and INA228, the INA237 has no `DEVICE_ID` register, so this check
confirms a TI part of this family but cannot distinguish the exact variant.

### `read [address]`

Reads the measurement registers and reports bus voltage, current and power,
along with shunt voltage and die temperature. With no address, every configured
monitor is read. With an address that has not been configured, the monitor is
registered on the spot using the default shunt.

The `DIAG_ALRT` register is checked on every read: an arithmetic overflow
(`MATHOF`) or a trim-memory checksum error (`MEMSTAT`) is reported, because
either one means the reported values cannot be trusted. `SHUNT_CAL` is also
read back, so a device that has reset since it was configured is flagged
instead of reporting mis-scaled current.

### `list`

Shows the configured monitors with their shunt resistance, current resolution
and full-scale range.

## SHT4x

Reached as `i2c-sht4x ...`. Drives Sensirion SHT4x humidity and temperature
sensors. The address is fixed by the part variant — `0x44` for the A variant
(such as the SHT40-AD1B), `0x45` for B and `0x46` for C — so every command takes
an optional address that defaults to **0x44**.

Unlike the INA237 the SHT4x has no registers. A command byte is written, the
sensor is given time to measure, and the result is read back in a separate
transaction; reading too early makes the sensor NACK. Each 16-bit value carries
its own CRC-8, which is checked on every read, so corrupted data is reported
rather than converted into a plausible-looking measurement.

### `read [address] [high|medium|low]`

Measures temperature and relative humidity, reporting both in engineering units
along with the raw tick values. Repeatability defaults to `high`; lower settings
are faster and noisier. Humidity is cropped to the physical 0–100 %RH range, and
if cropping was necessary the uncropped value is shown too — during bringup a
wildly out-of-range reading is a signal, not noise.

### `serial [address]`

Reads the sensor's 32-bit serial number. The SHT4x has no ID register, so a
serial number that reads back with valid CRCs is the available evidence that a
real sensor is responding.

### `heater [address] <mW> <ms>`

Pulses the on-die heater and then reports the measurement the sensor takes just
before switching it off. Power is 20, 110 or 200 mW and duration is 100 or
1000 ms; only those six combinations exist in the device. Useful for driving off
condensation, and for confirming the part responds to a stimulus.

### `reset [address]`

Issues a soft reset.

## NAU7802

Reached as `i2c-nau7802 ...`. Drives a Nuvoton NAU7802 24-bit bridge ADC as a
load cell front end. The address `0x2a` is fixed in silicon — there are no
address pins, so only one can be present per bus.

The device driver itself lives in [`components/nau7802`](../components/nau7802),
a reusable ESP-IDF component with no dependency on this console; `main/i2c/nau7802_cmd.c`
is the half that parses these commands and prints their output. Nothing about
the commands below changed in that split, but the register-level notes now live
with the component.

The normal bringup sequence is:

```
i2c-nau7802 init drdy 7     # power up, self-calibrate, DRDY on GPIO 7
i2c-nau7802 gain 128        # typical for a load cell's few mV of output
i2c-nau7802 tare            # with the scale empty
i2c-nau7802 calibrate 100   # with a known 100-unit mass on it
i2c-nau7802 weight          # thereafter, in those units
```

### `init [ldo <volts>] [drdy <pin>] [gain <1..128>] [scale <counts_per_unit>]`

Resets the device, applies the LDO voltage and gain, powers up the digital then
analog sections, waits for the power-up ready flag, lets AVDD settle, and runs
the internal offset calibration. The options describe how the board is wired,
so they may be given in any order.

`scale` installs a factor from an earlier bench calibration instead of measuring
one — see [below](#scale-counts_per_unit). It belongs on `init` rather than
being set afterwards: bring-up drops the tare and scale exactly as a gain change
does, so a factor set before `init` does not survive it. Giving it here also
ties it to the `gain` in the same line, which is the only way the two are ever
correct together.

**The order matters and is not the obvious one.** `VLDO` is written before
`AVDDS` turns the internal regulator on, because the register reset that opens
the command returns `CTRL1` to `0x00` — and `VLDO` `000` is 4.5 V, the top of
the range. Setting `AVDDS` first brought the regulator up at 4.5 V and only
then wound it down to the requested voltage, putting an overvoltage on AVDD for
as long as one I2C transaction takes. AVDD is then given 200 ms to settle
before the calibration measures against it: it is the converter's reference, so
calibrating into a moving supply produces an offset valid for a rail the part
is no longer running on.

By default AVDD is taken from the pin, which is the chip's own default.
Boards that rely on the internal regulator — many load cell breakouts do —
need `init ldo 3.0`. This is not the default deliberately: enabling the
internal regulator on a board that already drives AVDD would put two sources on
one net.

`drdy <pin>` names the GPIO the device's DRDY output is wired to; see
[`drdy`](#drdy-pinoff) below for what it buys you. Without it the driver polls
the CR status bit over I2C, which works but cannot start the read at a known
point in the conversion.

**`init` resets the gain to x1, and that is the trap on this part.** The command
opens with a register reset, so whatever `gain 128` was set to before is gone —
and nothing about the result looks wrong. The converter comes up, the offset
calibration passes, readings look plausible. They are simply 128 times smaller,
which puts a load cell's few millivolts down among the noise and makes
[`calibrate`](#read-samples-tare-samples-calibrate-known-mass-samples-weight-samples)
refuse for what appears to be no reason.

So `init` now always prints the gain it left behind, and says plainly when that
is the reset default rather than a choice you made:

```
> i2c-nau7802 init ldo 3.0 drdy 7
NAU7802 ready at 0x2A (device revision 0x0F)
Conversions signalled by DRDY on GPIO 7
AVDD from the internal LDO at 3.0 V
PGA gain x1
That is the chip's power-up default, not a choice this command made: 'init'
resets the registers, which returns the gain to x1 whatever it was set to
before. ...
```

Pass `gain 128` to set it in the same breath and skip the warning. `calibrate`
also names the gain when it refuses and the PGA is turned down, because
averaging harder cannot recover a factor of 128 and that is otherwise the only
advice on offer.

#### `init` writes `REG0x15 = 0x30`, and that is worth six bits

There is no option for this and no command to change it. §11.10 gives
`REG_CHPS[5:4]` — the CLK_CHP chopper clock — exactly one non-Reserved encoding,
`11` ("turned off, high (`1`) state"), and §9.1 *Power-On Sequencing* step 4
spells out the write as a whole byte:

> 4. At this point, all appropriate device selections and configuration can be made.
>    a. For example R0x00 = 0xAE
>    b. **R0x15 = 0x30**

The part powers up at `00`, and leaving it there costs **six bits of
resolution**. Measured on the sensor board, inputs shorted at mid-rail, internal
LDO 3.0 V, gain 128, 10 SPS, radio off:

| | CHPS 0 | CHPS 3 |
|---|---|---|
| RMS | 3883 counts | **59.9** |
| peak-to-peak | 22604 | **324** |
| effective bits | 12.1 | **18.1** |
| noise-free bits | 9.5 | **15.7** |
| input-referred | 5425 nV | **84 nV** |

65× on RMS, 70× peak-to-peak, reproducible on every toggle (2726 / 61.5 / 3082
counts RMS across `0` / `3` / `0`). The quiet state is a live converter, not the
silent dead one an unpowered AVDD gives — `OCAL` read −467, samples varied, and
the noise still tracked the sample rate (60.1 / 81.3 / 114.6 / 198.7 RMS at
10 / 20 / 40 / 80 SPS).

The write goes in with the rest of the configuration, before the settle delay
and the calibration, because the calibration has to measure the path the device
will actually convert with. `init` reads it back and says so, and
[`status`](#status) decodes it on every call — a chopper left at `00` is
otherwise invisible, since the converter still works and the numbers still look
plausible.

**Why this took so long to find.** The register was deliberately left alone for
most of this driver's life, because writing `0x30` railed the converter at
negative full scale on every rate. That observation was real; the conclusion was
not. The write went in with no recalibration and no settling discard afterwards,
and a stale calibration against a changed modulator is exactly what a railed
reading looks like. Once every analog-path change went through a
[recalibrate-and-settle path](#changing-the-analog-path), the identical write
was safe.

The chopper sits at the PGA *output*, which explains the shape of the noise it
caused: flat in counts across gain 1 to 128, and untouched by anything upstream
of the input pins — load cell, cable, resistor bridge, or a dead short. A long
hunt through the supply, the reference, the regulator and the board's decoupling
found nothing, because nothing there was wrong. It was found by diffing this
driver against the SparkFun and Adafruit libraries and noticing this was the
only register they wrote that it did not.

### `drdy [<pin>|off]`

Shows or sets the GPIO wired to the device's DRDY output, or releases it with
`off`. Unlike the other commands this one needs neither `init` nor a bus — it
is a statement about how the board is wired, so it can be declared or revoked
without disturbing a converter that is already running.

**What it closes.** The NAU7802 writes its three result registers straight from
the conversion, with no shadow register and no read latch, and it does not care
that an I2C transaction is in flight — bus atomicity is not register
atomicity. A burst read that straddles the moment the device updates those
registers would return the top byte of one conversion stitched to the low bytes
of the next.

Without DRDY the driver polls CR on the FreeRTOS tick, so it learns a
conversion is ready anywhere in a 10–20 ms window after the fact. At 40 SPS and
above that is a whole conversion period or more, and the read then begins at an
unknown phase — potentially as the registers are being rewritten. Waiting on
the pin instead begins the read microseconds after the write, with most of a
conversion period of margin.

A stitched read has a distinctive signature: near zero the result alternates
between `0x0000xx` and `0xFFFFxx`, so the top byte comes from the wrong sample
and the value lands about ±65,500 away from its neighbours. **That signature
has never been observed on the sensor board** — runs of 250 samples at gain 1,
where the signal straddles zero and the top byte flips 40–60 times, produced
zero such outliers with polling or with DRDY. Treat this as a window that is
closed on principle, not as an explanation for a noisy reading; if readings are
noisy, the cause is somewhere else.

A wrong pin number fails cleanly rather than quietly: the input is pulled down,
so a pin that is not connected to DRDY reads low, times out, and says so. A
floating input left to sit high would instead look permanently ready and
return bad numbers.

```
i2c-nau7802 drdy 7          # declare it
i2c-nau7802 drdy            # show it, and the line's level right now
i2c-nau7802 drdy off        # back to polling over I2C
```

One read still carries the old timing risk: if DRDY is already high when a
measurement starts, a result is sitting unread and there is no edge coming —
DRDY does not fall until the result is read — so the driver takes it at an
unknown phase. That can only be the first sample of a batch, since reading a
result drops the line and re-arms the edge for every sample after it.

### `status`

Reads `PU_CTRL`, `CTRL1`, `CTRL2` and the revision register back from the part
and reports each one as both the raw byte and what its bits mean, followed by
the tare and scale this session is holding.

Output after `init ldo 3.0` and `gain 128`:

```
Device revision 0x0F at 0x2A
PU_CTRL 0xBE  digital up, analog up, ready yes, data ready
AVDD source: internal LDO
CTRL1   0x2F  gain x128, LDO 3.0 V
CTRL2   0x00  10 SPS, calibration ok
Input channel: A
Channel A calibration: OCAL 0x800275 (-629 counts), GCAL 0x00800000 (x1.000000)
Data ready: DRDY on GPIO 7, now low
Tare 8421 counts; calibrated
Scale 214.7 counts per unit (measured this session)
    scale factor for the consumer:  214.7  /* gain x128, counts per unit */
```

The second scale line is there to be copied. It prints at `%.17g` — seventeen
significant digits, the width at which a `double` survives text and comes back
bit-identical — so pasting it into a consumer's source, or back into `scale`,
reproduces the run exactly. The human-readable `%.1f` above it does not: at one
decimal place, pasting the number back and asking for it again prints something
different, and nothing would tell you whether that was rounding or a bug.

In practice the line is usually far shorter than seventeen digits, because this
build's picolibc emits the shortest string that still round-trips. `214.7` prints
as `214.7`, not `214.69999999999999`; a factor that genuinely needs the digits
gets them. Verified on the sensor board — `86213.854059609454` comes back as
`86213.85405960945`, which parses to the identical double.

**It does not require `init`, only a bus.** Every line above the tare is read
out of the chip, so this reports how the part is *actually* configured rather
than what this firmware believes it did — which is the whole point after the
ESP has been reset while the NAU7802 kept its power, or when something else set
the device up. Without `init` in this session the last two lines are replaced
by a note saying so; the register lines are still true.

That split is worth keeping in mind: the registers live in silicon, while the
tare offset and scale factor live only in this firmware's memory. A soft reset
of the ESP loses the calibration while leaving the chip configured and
converting, and `status` is how you see that state.

During bringup the fields that usually explain a problem are `ready` and
`data`: `ready no` means the analog section never came up, and `data pending`
means no conversion has completed, which on a part that is otherwise powered
points at conversions never having been started. `calibration ERROR` is the
chip's own `CAL_ERR` bit — the internal offset calibration failed, and every
reading after it is untrustworthy. `AVDD source` catches the wiring mistake
`init` is careful about: a board that feeds AVDD from a pin, reported here as
running from the internal LDO, has two sources on one net.

The calibration line reports what the internal calibration actually chose for
the active channel, read out of `OCAL`/`GCAL`. `CAL_ERR` above is one bit and
only says the device gave up; `OCAL` is the offset in ADC counts it settled on,
which is the number that says whether the front end is anywhere near balanced.
Close to zero means the bridge sits near mid-supply. Up against the rails means
it does not, and the gain has nowhere left to go — a part in that state passes
its calibration and then saturates under load. Each channel has its own
`OCAL`/`GCAL` pair, and the internal calibration writes whichever `CTRL2.CHS`
selects, which is why switching channels needs its own calibration rather than
inheriting the other one's.

**`OCAL` is sign-and-magnitude, not two's complement** — bit 23 is the sign and
the low 23 bits the magnitude. That is worth stating because the result
registers a few addresses away *are* two's complement, and the datasheet does
not spell the difference out. Six calibrations on the sensor board read
`0x000F58`, `0x800622`, `0x80000C`, `0x001D82`, `0x000792`, `0x8006F9`. As
sign-magnitude those are +3928, −1570, −12, +7554, +1938, −1785: small offsets
scattered either side of zero, which is what an offset calibration produces.
Decoded as two's complement, the three with bit 23 set all come out around
−8.38 million — three independent calibrations apparently landing on the rail
while the device reports `CAL_ERR` clear. The first version of this read-back
did exactly that and made a working part look broken.

### Changing the analog path

`gain`, `rate`, `input`, `ldomode`, `pgacap` and a channel switch inside `raw`
all change what the converter is measuring or how. Every one of them goes
through the same three steps, because re-running the device's internal offset
calibration — which is what these commands used to do, and only that — is
necessary but not sufficient.

**1. Recalibrate.** `CALMOD` `00`, the internal offset calibration, against the
new configuration. `CAL_ERR` is cleared in the same write that starts the run:
the verdict is read back out of the register that reported the *previous*
calibration, so a stale error bit carried forward would make one failure
condemn every calibration after it.

**2. Flush and settle.** Four conversions are discarded. The first is stale
rather than merely unsettled — the device holds its last result in `ADCO` until
someone reads it, and neither the calibration nor the register write that
prompted it clears that, so `DRDY` stays high, `CR` stays set, and the next read
returns *immediately* with a conversion that completed under the old gain, rate
or channel. That is not a bad sample but the previous configuration's good one,
which is worse, because it looks right. The remaining three are filter history:
the converter is sigma-delta, so its output depends on several preceding
modulator cycles.

The worst case this closes was `raw 5 b`: switch to channel B, calibrate, and
print channel A's old value as sample 1.

**3. Drop the tare and scale.** Done first, in fact, and unconditionally: These live in this firmware, not in the chip,
and the device knows nothing about them. They were measured at the old gain and
are wrong by exactly the factor nobody will notice — `calibrate 100` at x1
followed by `gain 128` left `weight` reporting a hundred and twenty-eighth of
the true mass, self-consistently and with a plausible error bar. Losing a
calibration loudly beats keeping one that quietly reports the wrong weight, so
the commands say what they dropped — including when the calibration or the
settling read fails partway, which is exactly where leaving a plausible-looking
scale factor behind would do the most damage.

The conversion cycle is also re-asserted (`PU_CTRL.CS`) after each calibration,
and the command says so if it had in fact stopped. `init` sets `CS` *after*
calibrating, which reads as though calibration is expected to leave the cycle
stopped — **measured on the sensor board, it is not.** `PU_CTRL` comes back
`0xBE`, `CS` set, after every one of `gain`, `rate` and `input`, and the message
has yet to appear. `init` sets `CS` there because nothing had set it since the
register reset, not because `CALS` cleared it.

The check is kept anyway: it is one transaction, it is a no-op on this silicon,
and it is the difference between "conversions stopped" being a hang and being a
line of output on some other board or revision.

`init` does not take this path: it has no conversions to flush and clears the
scale itself, though it still discards three conversions for a filter starting
with no history at all.

**A factor supplied with [`scale`](#scale-counts_per_unit) is dropped by all of
this too, and that is deliberate.** A scale factor is counts per unit *at one
gain*, so a gain change leaves it wrong by exactly the gain ratio — the same
self-consistent error the flush exists to prevent, and worse for a compiled-in
constant than a measured one, because whoever compiled it in believes it is a
property of the board. The advice differs, though, so the commands say which
kind of factor they dropped: a measured one is re-measured here, while a
supplied one has to come from a bench run at the gain now in force. To set both
at once and have the factor survive, use `init … gain <n> scale <K>`.

### `gain [1..128]`, `rate [10|20|40|80|320]` and `input [a|b]`

Show or set the PGA gain and conversion rate. Both go through the settling path
described under [Changing the analog path](#changing-the-analog-path), because
either change alters the analog path and invalidates the existing calibration —
without that, readings come back swinging across most of the full-scale range.

**320 SPS is not usable on the board this was developed against.** It returns
values spanning the entire range regardless of calibration, while 10–80 SPS are
rock steady; the likely cause is that 320 SPS needs an external crystal rather
than the internal RC oscillator. The command warns when you select it.

`input` shows or selects the converter's channel, A or B. It goes through the
same settling path for the same reason: the two channels have their own offset
and gain calibration registers, so a switch invalidates a tare taken on the
other one. Note that `ldomode 1` consumes channel 2's pins for the filter node,
which is why `input b` and that setting are mutually exclusive — see
[`ldomode`](#ldomode-01).

### `ldomode [0|1]`

Shows or sets `REG0x1B[6]`, which picks the compensation for the internal
regulator's control loop. It has to match the capacitor the board fits on AVDD:

| | AVDD capacitor | trade |
|---|---|---|
| `0` (chip default) | ESR **below 1 Ω** | better DC accuracy, higher loop gain |
| `1` | ESR **up to 5 Ω** | more stable loop, lower DC gain |

Pin 16's description in the data sheet asks for "low ESR 1 ohm or less" because
that is what the default expects. A board fitting something with more ESR than
that and leaving this bit alone runs a marginally compensated regulator, which
would be a broadband noise source on the supply and the reference — after the
PGA, where no amount of gain or input rewiring reaches it.

Worth checking on an unfamiliar board; on the sensor board it makes no
measurable difference, so whatever is on AVDD there is comfortably inside the
default's 1 Ω.

Changing it takes the [analog path](#changing-the-analog-path) route, since the
two modes settle AVDD — the reference — at slightly different levels.

### `pgacap [on|off]`

Shows or sets `REG0x1C[7]`, which connects a filter capacitor across the
VIN2P/VIN2N pins to the PGA output. The data sheet offers it "for enhanced ENOB
at high PGA gain settings".

Two things it needs. The capacitor has to be physically fitted — **330 pF at
AVDD 3.3 V, 680 pF at 4.5 V** — and with none there this bit changes nothing at
all. And it consumes channel 2, whose pins become the filter node, so `input b`
after enabling it reads the capacitor rather than an input.

On the sensor board it changes nothing, which is the expected result for a
board with no such capacitor fitted. Either way it takes the
[analog path](#changing-the-analog-path) route, since enabling it changes what
the PGA drives.

### `raw [samples] [a|b]` and `registers`

`raw` prints individual conversions with no averaging, tare or scale applied,
as a percentage of full scale alongside the count. The optional channel
argument switches inputs **only if the device is not already on the one asked
for**: it used to rewrite `CHS` and recalibrate on every invocation, which cost
a calibration and its settling discards even when nothing changed and, because
it recalibrated, silently threw away the tare and scale that a diagnostic read
has no business touching.

`registers` dumps the whole map — `PU_CTRL` through `DEVICE_REV`, named — which
means both calibration blocks, the result registers, and `PGA`/`POWER`. It
previously stopped at `0x0B`, in the middle of channel 2's offset calibration,
so it covered part of one calibration block, none of the other, and none of the
registers worth looking at during bringup. `0x15` reads back as the ADC register
so long as `REG0x1B[7] RD_OTP_SEL` is `0`, which is the default and which
nothing here changes; set that bit and the same address returns `OTP[32:24]`
instead.

### `read [samples]`, `tare [samples]`, `calibrate <known mass> [samples]`, `weight [samples]`

`read` reports the averaged raw count, the spread across the samples, and the
percentage of full scale. An absolute voltage is deliberately not reported: it
would depend on REFP−REFN, which this driver has no way to know.

Once a tare exists `read` adds the count net of it, and once `calibrate` has
run it adds the same figure in calibrated units, with the ± that `weight`
would quote. Counts stop being the number anyone wants from a load cell as
soon as there is a scale factor to apply, but `read` is still the command that
shows the raw one, so the converted figure goes alongside it rather than
replacing it — `weight` remains the command that reports only the load.

`tare` captures the zero offset, `calibrate` derives the scale factor from a
known mass, and `weight` reports the load in whatever unit was used to
calibrate. `weight` also reports the sample spread converted into those units,
so every reading carries an indication of its own noise.

Two guards worth knowing about. Any reading pinned at full scale is reported as
a saturation error rather than a large number — during bringup that usually
means the bridge is disconnected, unexcited or miswired. And `calibrate`
refuses when the reading has not moved clear of the noise, since calibrating
against noise yields an absurd scale factor that silently corrupts every later
weight.

**The noise guard tests the uncertainty of the averages, not the spread of the
samples**, and on a noisy part the difference decides whether you can calibrate
at all. Peak-to-peak spread *grows* with the sample count — more draws, more
chance of an extreme one — while the uncertainty of a mean *falls* as
1/√n. A guard written against the spread therefore gets harder to satisfy the
more you average, which makes "take more samples" — the one correct response to
a noisy part — actively counterproductive.

That is not hypothetical here. The NAU7802 on the sensor board ran at about
4800 counts RMS at gain 128, and a 100 g mass on a 2 kg cell is only around
107,000 counts (1 mV/V, 3.0 V excitation, ±11.7 mV full scale at gain 128).

> **Since resolved.** That 4800-count figure was the chopper clock left at its
> power-up value. `init` now writes `REG0x15 = 0x30` as the data sheet
> prescribes (see
> [above](#init-writes-reg0x15--0x30-and-that-is-worth-six-bits)) and the same
> board reads about 60 counts RMS. The guard below is still right and still earns its place — the
> arithmetic about spread versus standard error does not depend on how noisy the
> part is — but the numbers in the table are from the broken configuration, and
> a correctly configured part clears the guard with room to spare.

Measured on that board with nothing on the cell:

| samples | batch spread | old guard demanded | combined std. error | new guard demands |
|---|---|---|---|---|
| 10 | 13,123 | 131,000 | 1553 | 15,500 |
| 50 | 16,208 | 162,000 | 720 | 7,200 |
| 100 | 21,091 | 211,000 | 508 | 5,100 |

So 100 g could not be calibrated at *any* sample count, and averaging harder
made it worse — the middle column climbs while the one that actually matters
falls. The guard now requires the move to clear ten times the combined
standard error of the tare and the calibration measurement — the two averages
being subtracted, added in quadrature — which is the same as saying the scale
factor must be good to 10%.

`calibrate` then **reports the precision it achieved**, because the scale
factor's relative error is inherited by every weight taken afterwards, and
suggests averaging harder when it is worse than 1%. `tare` reports how well it
pinned its own mean for the same reason.

The HX711 driver ([`loadcell`](loadcell.md)) carries the identical guard. It
never tripped there — that part measures about 48 counts RMS on the same
board, a hundred times quieter — which is exactly why the flaw stayed hidden
until a noisy part met it.

### `scale [counts_per_unit]`

With no argument, reports the factor in force and where it came from. With one,
sets it — without measuring anything.

This is the other half of a factory calibration. `calibrate` is how the number
is *produced*: run it once on a bench, against a known mass, and keep what it
prints. `scale` is how that number gets back into a board that has no reason to
re-derive it every time it powers on.

```
> i2c-nau7802 calibrate 100 100
Calibrated: 214.7 counts per unit (21470.0 counts for 100.0000 units)
Scale is good to +/-0.08%, from 17.2 counts of uncertainty in the tare and this measurement together
    scale factor for the consumer:  214.7  /* +/-0.08%, gain x128, counts per unit */

> i2c-nau7802 scale 214.7
Scale set to 214.7 counts per unit (supplied, not measured)
No tare yet. Run 'tare' with the cell empty before weighing; a factor fixes the span, not the zero.
```

**Only the factor, never the tare.** A scale factor is a property of the cell
and the gain, and those do not change between boots. The tare is the bridge's
own zero, and it moves — with temperature, with mounting, with whatever is
bolted to the cell — so no constant compiled into a binary can stand in for it.
The flow is `init … scale <K>`, then `tare` with the cell empty, then `weight`.

`weight` refuses outright when a factor is set and no tare has been taken. That
combination is only reachable through this command: `calibrate` will not run
without a tare of at least two samples, so before `scale` existed a calibrated
scale always implied a real zero. Without one, the subtraction is against zero
while an unloaded bridge sits tens of thousands of counts away from it — and
that offset would be reported as load, confidently and wrongly.

**The ± on a weight does not cover a supplied factor.** It never covered a
measured one either: `weight` propagates this session's noise through a factor
treated as exact, and `calibrate`'s own `+/-0.08%` was never folded in. So the
error bar has always been repeatability rather than accuracy. That is easy to
misread, and most misleading where the firmware cannot know the number's
provenance at all, so `status` says so whenever the factor was supplied. The
accuracy travels the other way instead — in the comment on the pasteable line,
which is the only moment the firmware knows it.

A supplied factor is dropped by every command that touches the analog path, and
by `init`. See [Changing the analog path](#changing-the-analog-path).

---

# The other I2C parts

Each part below is its own group, `i2c-<part>`. They all take a device handle
from the `i2c` group's cache, so `i2c bus <scl> <sda>` comes first; and they all
re-fetch that handle before every command, because `i2c bus` deletes every
cached handle when it re-creates the bus.

## INA219

`i2c-ina219`, at 0x40–0x4f. The oldest of the three current monitors here and
the one that identifies itself least well.

### `config <address> [shunt_ohms] [max_amps]`

Registers a part, programs the calibration, and reports the resolution it
actually got. Defaults are 0.1 Ω over 3.2 A, which is the part's reference case:
3.2 A across 0.1 Ω is 320 mV, exactly the widest range the PGA offers.

`max_amps` is not decoration. The current register is signed 15-bit, so the
resolution is `max_amps / 32768`; asking for more range than needed throws
resolution away, and asking for less saturates. The product `shunt_ohms ×
max_amps` must land between about 20 mV and 320 mV or the request is refused,
because beyond 320 mV the front end saturates whatever the calibration says.

The reported resolution is rarely the round number asked for. The calibration
register's low bit is void — an odd value cannot be written — so the part stores
an even one and the resolution follows from that, not from the request.

**There is no ID register.** A wrong part is inferred by writing the calibration
register and reading it back, so "does not behave like an INA219" is the
strongest identification available. Compare the INA226, which has both a
manufacturer and a die ID.

### `read [address]` and `list`

`read` with no address reads every configured part; with one, it reads that part,
registering it at the defaults first if it is not configured. `list` tabulates
the configured parts with their shunt, resolution, full scale and calibration
register.

## INA226

`i2c-ina226`, at 0x40–0x4f.

### `config <address> [shunt_ohms] [max_amps] [avg_samples]`

As the INA219, with two differences that are the part's own.

The shunt input is ±81.92 mV and there is **no PGA** to widen it, so the range
check is tighter: `shunt_ohms × max_amps` must fall between about 5.12 mV and
81.92 mV. The defaults, 0.01 Ω over 8.192 A, use exactly all of it.

Averaging is done in hardware — 1, 4, 16, 64, 128, 256, 512 or 1024 samples,
defaulting to 16. It is the setting that matters most on a noisy rail, and it
trades response time for noise; a zero in the driver's config means no averaging
at all, so this group asks for 16 rather than inheriting that.

Identification is real here: manufacturer 0x5449 and die 0x2260 are read back and
both are named when they do not match. Pointing this at an INA237 reports
`manufacturer 0x5449, die 0x2381` — TI, but the wrong die — which is a more
useful answer than a bus error.

### `read [address]` and `list`

As the INA219's, plus the averaging in use.

## LM75B

`i2c-lm75bdp`, at 0x48–0x4f. A temperature sensor with a thermal watchdog output.

### `read [address]`

Temperature in Celsius and Fahrenheit, at the part's 0.125 °C resolution.

### `limits [address] <tos_C> <thyst_C>`

Programs the watchdog. The OS output asserts above `tos` and releases below
`thyst`; the gap between them is the hysteresis, and equal values make the output
chatter around the threshold, so `thyst` above `tos` is refused.

**The reply says what was programmed, not what was asked for.** Thresholds
quantise to 0.5 °C and clamp to −128…+127.5 °C, and when the two differ the
requested values are echoed alongside so the difference is visible rather than
silent.

## RX8130CE

`i2c-rx8130ce`, at the fixed address 0x32. A real-time clock with battery backup.

### `time`

Reads the clock, prints the system time beside it and the drift between them.

The interesting outcome is the failure. **A clock that does not know the time
says so**: a part whose backup cell has drained still answers and still returns
registers, they simply do not decode to a date. That is reported as such rather
than as a time, because returning it would let a dead RTC pass for a device that
believes it is the year 2000. Separately, the part's own voltage-low flag is
reported when set — the time decodes, but the part is telling you not to trust
it.

The part holds UTC, and so does this display.

### `set`

Copies the system clock into the RTC, and refuses if the system clock has never
been set — there is nothing to copy, and writing an epoch date would clear the
power-lost flag while leaving the clock wrong, which is the worst of both.

Setting the time clears the voltage-low flags, so a clock that has been set stops
reporting itself unreliable.

## AW9523B

`i2c-aw9523b`, at 0x58–0x5b. Sixteen I/O pins in two ports behind I2C — pins the
[`gpio`](gpio.md) group cannot reach, which is the whole reason this group
exists.

### `init [address] [p0in <mask>] [p1in <mask>] [p0init <mask>] [p1init <mask>] [pushpull]`

**Every pin is an input unless a mask says otherwise.** That is the part's own
reset state and the only safe assumption on a board whose pinout is not yet
known: an output driven into an unknown net is how a bring-up session damages
hardware. A 1 bit in `p0in`/`p1in` is an input, matching the part's register.

`p0init`/`p1init` are the levels established **before** any pin becomes an
output. The ordering is the point: switching direction first lets whatever the
output register happened to hold reach the pins, which on a board driving relays
is an audible clack at every reset and on one driving FETs can be worse.

**Port 0 is open-drain unless `pushpull` is given.** Port 1 is always push-pull.
This surprises anyone expecting a GPIO to source current.

Identification is the chip ID, 0x23.

### `read [port]`

Pin levels for one port or both, in hex and binary with P*n*.7 first.

These are **pin levels, not the output register** — the part has no readable
output register. On an output pin the level can differ from what was driven, if
something else is holding it.

### `write <port> <value>` and `set <port> <pin> <0|1>`

Drive a whole port, or one pin. Both work from the driver's shadow of the last
value written rather than a read-modify-write, because reading gives input levels
and folding those back would write pin state over the outputs.

Driving a pin that is configured as an input says so instead of failing silently,
and names the mask to change.

## PI4IOE5V6408

`i2c-pi4ioe`, at 0x43 or 0x44. Eight pins, 5 V tolerant, with configurable pulls
and interrupt-on-change — which is why it is here alongside the AW9523B rather
than one standing in for the other.

### `init [address] [out <mask>] [init <mask>] [pull <mask>] [pullup <mask>] [int <mask>]`

Same defaults and same reasoning as the AW9523B, with the sense of the direction
mask inverted to match this part's register: a 1 bit in `out` makes the pin an
**output**.

`pull` turns a pull resistor on and `pullup` chooses its direction. A switch to
ground with no external pull-up needs its bit set in **both**, which is the usual
case for a button. `int` selects which pins report changes through the INT
output.

### `read`, `write <value>`, `set <pin> <0|1>`

As the AW9523B's, over one eight-bit port.

### `interrupt`

Which pins have changed since this was last read. **Reading the register clears
it**, so the next call reports only what changes from here.

Several parts sharing one open-drain INT line is what the `int_dispatch`
component is for; this command is the manual equivalent, for finding out whether
the line works at all.
