# Audio

[← Command reference](README.md) · [Project README](../README.md)

Plays a tone or a frequency sweep out of an I2S codec or amplifier, captures
from a microphone or an ADC, and reports what the hardware actually did with
either.

This is a capability menu rather than a bus menu — it owns the I2S transport
the way `sd` owns whichever bus a card is on. The generic commands here never
mention a specific part: they work through a small codec interface, so a
NAU8822 with sixty registers and an NS4168 with one enable pin are driven by
the same `audio tone`, `audio volume` and `audio mute`. Parts appear as
submenus (`audio-nau8822`, `audio-ns4168`, `audio-sph0645`) and attach
themselves when initialized.

**Two parts can be attached at once, one per direction.** A board may well have
an amplifier and a microphone on the same bus, and testing one against the
other is the whole point of `audio loopback`. Attaching a part only displaces
one that needs the same direction; a part that works both ways, like the
NAU8822, occupies both slots on its own. A single slot would be worse than
limiting — detaching powers a part down, so attaching a microphone would
silently stop the amplifier.

**The wire is always two slots wide.** I2S has a genuine mono slot mode, but a
stereo part fed one-slot frames plays everything an octave down at half speed,
and every part this targets expects a two-slot frame. So there is no mono
option; put the signal in one channel with `left` or `right` instead, which is
also how you find out which slot a mono amplifier is listening to.

## `bus <bclk> <ws> <dout> [din <pin>] [mclk <pin>] [rate <hz>] [bits <16|24|32>] [mclkmult <n>]`

Initializes I2S on the given pins. The rate defaults to 48000 Hz and the width
to 16 bits. Re-running it tears the old configuration down first, so pins and
format can be changed freely; an attached codec is reconfigured to match.

`mclk` is needed by codecs that cannot synthesise a clock from BCLK alone — the
NAU8822 is one — and unnecessary for a simple amplifier. `mclkmult` sets MCLK
as a multiple of the sample rate; it defaults to 256, or 384 at 24 bits, which
the driver requires to be a multiple of 3 for the rate to come out accurate.

`din` adds a receiver sharing the same BCLK and WS — an I2S microphone such as
the SPH0645 (which has a submenu of its own, below), or a codec's ADC. A PDM
microphone is a different mode and has its own command, `audio pdm`.

**Setting `din` to the same pin as `dout` is an internal loopback.** The I2S
driver notices and feeds the transmitter straight back to the receiver through
the pad, with nothing connected. That is the one capture test that needs no
hardware at all, which makes it the right first move when a microphone reads
silent: it separates a broken receive path from a broken part.

**The transmitter starts immediately and keeps running.** With nothing queued
the DMA sends silence, so BCLK and WS are live from this point on. That is what
a codec wants — most of them mute or reset when their clocks stop, and starting
and stopping the clock around every tone makes an amplifier pop — and it means
the lines can be put on a scope before any signal is played.

The clocks are reported as configured, not as requested:

```
> audio bus 41 43 42
I2S initialized on BCLK=41, WS=43, DOUT=42
Format:  48000 Hz requested, 48000 Hz configured, 16-bit, 2 slots
Clocks:  BCLK 1.536 MHz, MCLK not routed to a pin
Transmitting silence, so the clocks are live for the codec.
```

## `info`

Shows the pins, format, clocks, whether output is running, and the attached
codec with its own status.

## `codecs`

Lists the parts this firmware knows how to drive, which direction each works
in, and whether it needs MCLK.

## `tone <hz> [seconds|continuous] [level <pct>] [left|right|both]`

Plays a sine tone. The duration defaults to 3 seconds and the level to 25 % of
full scale (−12 dBFS) — loud enough to hear, quiet enough not to hurt.

`continuous` is the interruptible form. Commands execute one at a time, so a
finite tone occupies the shell — serial *and* web — for its whole duration;
a continuous one runs on its own task and is ended with `audio stop`.

The signal comes from the `tone_gen` component, from a 129-entry quarter-wave
sine table with interpolation rather than by calling `sinf()`. The S3 has an FPU
and would manage; the C3 has none, and softfloat there would spend a large part
of a core generating the test signal, which is precisely when the question is
whether the *hardware* keeps up. The table costs 258 bytes of flash and no RAM,
and its error is about 1 LSB of 32767 — far under anything an amplifier under
test contributes. Phase is a full 32-bit accumulator, so no frequency has to
divide into the buffer length and block boundaries are inaudible, and every tone
gets a 5 ms raised-cosine fade at each end so it does not thump the speaker.

```
> audio tone 1000 3
Played 1000.0 Hz at 25% (-12.0 dBFS), 144000 frames
Sample rate: 48000 requested, 48000 configured, 47999 measured
```

**The measured rate is the one worth reading.** The configured rate says what
the divider was set to; the measured one says how fast samples actually left.
It is timed between two moments when the DMA queue is equally full, so the
buffer cancels out — timing the whole run instead would charge the prefill to
the measurement and under-report by the buffer depth. A shortfall means
something starved the DMA.

## `sweep <start_hz> <end_hz> [seconds] [level <pct>] [log|linear] [left|right|both]`

Sweeps between two frequencies over 5 seconds by default. Logarithmic unless
told otherwise, because equal time per octave is how audio hardware is judged.
The frequency steps every 5 ms, whatever the sample rate, with phase carried
across, so there is no clicking at the steps.

## `dtmf <digits> [tone <ms>] [gap <ms>] [pause <ms>] [level <pct>] [twist <db>] [left|right|both]`

Dials a string of touch-tone keys: each is two sine tones at once, one from a
low group and one from a high group, so no single frequency can be mistaken for
a key.

```
          1209  1336  1477  1633 Hz
    697     1     2     3     A
    770     4     5     6     B
    852     7     8     9     C
    941     *     0     #     D
```

Keys are `0`–`9`, `*`, `#` and `A`–`D` (either case). `,` is a pause of
`pause` milliseconds, for a line that needs time to answer. A space or `-` is
skipped, so `555-1234` dials as `5551234`. A character that is none of these is
refused before anything plays, with its position.

The usual use is dialling a phone through a speaker held to the handset, which
wants the sample rate the telephone network uses:

```
> audio bus 41 43 42 din 46 rate 8000
> audio dtmf 5551234
Dialled 5551234
Keys 100 ms, gaps 100 ms, pauses 2000 ms
Level 25% (-12.0 dBFS), twist 2.0 dB, 10400 frames
Sample rate: 8000 requested, 8000 configured, 8000 measured
```

Any bus rate works; 8 kHz is simply the one that costs least and matches the
line. A DTMF receiver needs 40 ms or more of each key and of each gap, which is
why the defaults are 100 ms; `tone` and `gap` go down to 20 ms to find a
receiver's limit.

**Level is the two tones together.** `level` is the combined peak, as a percentage
of full scale, so the sum can never clip. `twist` makes the high tone louder than
the low one by that many decibels (0–6, default 2): telephone lines lose more at
high frequency, and receivers are specified to expect the high tone a little
stronger. Each key has a 3 ms raised-cosine edge, so keys do not click.

Like a finite `tone`, `dtmf` occupies the shell until it finishes, and always
does — there is no continuous form.

## `stop`

Ends a continuous tone, ramping it down rather than cutting it.

## `pdm <clk> <data1>[,<data2>,<data3>,<data4>] [rate <hz>] [ultra] [drive <0-3>] [clkinv]`

Opens a PDM microphone — the SPM1423 on the Cardputer, the one on the XIAO
Sense. PDM is not I2S with different pins: the part is clocked at a few
megahertz on a single line and sends one bit per clock, and the peripheral's
decimation filter turns that back into PCM. So it gets its own receiver rather
than joining the standard bus, and its own command.

The rate is the *decimated* rate, and it sets the microphone's own clock at 64
times itself — 48 kHz gives 3.072 MHz. That matters, because a MEMS PDM part
typically specifies 1 to 3.25 MHz and drops into a low-power mode below that,
where it looks exactly like a dead microphone. The command reports the clock it
produced and says so when it lands outside that range.

If the standard bus is already open, the microphone inherits its sample rate.
Capture and playback share one rate deliberately: a loopback measured at two
different rates would not mean anything.

**Several data lines.** On the ESP32-S3, `audio pdm 10 11,12 rate 32000` takes two
lines on one clock, each carrying a left and a right microphone, so four channels in the
order the chip delivers them: line 1's two slots, then line 2's. Up to four lines
(eight microphones) are allowed. They share the one filter and so one rate. The chip's
documentation says the right slot of a line arrives first. This was verified on a board
with four microphones on GPIO 11 and 12 and streams over [`audio-usb`](#usb-audio);
`record`, `level` and `capture` still read the first two channels only.

**`ultra`** is for ultrasonic MEMS parts (the SPH0641): they must wake in standard mode
at 1 to 2.475 MHz and have the clock raised to 3.072-4.8 MHz on the same running
receiver, so `ultra` opens at 2.048 MHz, lets the part run for 100 ms, and reclocks the
receiver in place to 64 times `rate`. The filter's ratio is fixed, so `rate` must be
48000 to 75000 (a 3.072 to 4.8 MHz clock), and the output is band-limited to half of
that: the way to get several ultrasonic microphones into one stream, but not their
ultrasonic content, which needs `pdmultra`'s software filter and so one line.
`audio pdm 10 11,12 rate 75000 ultra` is four ultrasonic-mode channels at 75 kHz. It
cannot join an open standard bus, which sets its own clock.

**Long cables.** Noise that rises steadily with the clock, worse on the right slot (the
one sampled on the opposite edge), is a signal-integrity symptom, not the microphones:
four microphones on 1 m cables were clean at 2.048 MHz, noisy by 3.584 MHz and saturated
at 4.8 MHz. `drive <0-3>` sets the clock pin's drive strength (0 weakest: slower edges
that couple less into the data lines) and `clkinv` inverts the clock the microphones see,
which moves the sampling phase against their data. Both apply to this open only. With
`ultra`, and in `pdmultra`, the clock pin already runs at drive 3: with the default drive,
four microphones on 1 m cables were saturated at 4.8 MHz (thousands of LSB of noise), and
with `drive 3` or `clkinv` they were clean (14-24 LSB) at 48, 64 and 75 kHz.

```
> audio pdm 43 46
Input:   PDM on CLK=43, DATA=46, decimated to 48000 Hz 16-bit
         Microphone clock 3.072 MHz
         Receiver running
```

**A PDM microphone has no control interface, so it has no driver here.** There
are no registers, no address, no enable — the transport is the whole of it.
That is why there is no `audio-spm1423` group to match `audio-ns4168`: the
NS4168 earned a driver by having one pin, and this part does not have even
that. The same applies to an I2S microphone, which `audio bus ... din <pin>`
covers outright.

On the ESP32-C3 this refuses. The chip can clock a PDM microphone but has no
PDM-to-PCM filter, so what arrives is the raw one-bit stream; every statistic
below would be measuring the modulator rather than the sound, and would look
like plausible numbers while meaning nothing.

## `pdmultra <clk> <data> [maxhz <hz>] [sinc6] [raw]`

Opens an ultrasonic-capable PDM microphone (written for the Knowles
SPH0641LU4H-1) and decimates it in software. The chip's hardware filter divides
by 64, which at the part's 4.8 MHz ultrasonic clock is 75 kHz PCM; this takes
the raw bits instead and filters them with a sixth-order sinc.

The part must not start in ultrasonic mode, so the command clocks it at
2.048 MHz first, waits 100 ms, then raises the clock to 4.8 MHz on the same
open channel (the pins are never released).

`raw` stops after the start-up sequence: the receiver is left as a raw PDM input
at 4.8 MHz, undecimated, for [`pdmbits`](#pdmbits-seconds--dump-frames-grab-seconds)
`dump` or `grab`. It is how to capture the bitstream with the part in its
ultrasonic mode, which `pdmraw` cannot do (it would start the part at 4.8 MHz).
`maxhz` and `sinc6` have no effect with it.

The default decimator is the two-stage one (sinc^4 over bytes, then a 24-tap
FIR on the S3's PIE unit): always 4.8 MHz to 300 kHz, about 17% of a core.
`sinc6` selects the original sixth-order sinc instead, about 33%, whose ratio
follows `maxhz` (so `maxhz 40000` gives 150 kHz PCM and a lower load). The two
are different filters; see [`pdmbench`](#pdmbench-frames-channels) for the numbers.

`maxhz` is the highest frequency you care about, 80000 by default (the part's
specified limit). With `sinc6` it picks the decimation ratio — the largest whose
Nyquist is at least 1.5 x `maxhz`, so 80 kHz gives 300 kHz PCM — and in either
mode it limits the spectrum that [`record`](#record-seconds) reports, so bands above it are not shown. It
may not exceed 100000.

**It needs the CPU at 240 MHz.** At 300 kHz the receiver delivers 1.2 MB/s
and the decimator plus the capture statistics run once per word; at the
S3's former default of 160 MHz the DMA overruns without any error, and the
symptom is a noise floor 20 dB too high that falls as `maxhz` falls. The S3
defaults now set 240 MHz (`sdkconfig.defaults.esp32s3`). the
`pdm_decimator` component and `capture.c` are built `-O2` for the same reason.

The mic clock is fixed at 4.8 MHz, the top of the part's ultrasonic range, and
`audio pdmbits` / `pdmraw` is the way to try other clocks. Raw mode cannot
clock faster than about 5 MHz (the driver needs 16 times the bit clock below
80 MHz); the hardware-filtered `pdm` path reaches 6.66 MHz but only divides by
64, which is why `pdmultra` exists.

`record` prints the decimator's cost under the capture table: cycles per
output frame and that as a share of one core. With FreeRTOS run-time stats
enabled it adds the capture task's whole share (statistics and I2S read too).

Everything that takes decimated PCM (`record`, `level`) works on it. The sinc
filter droops, about 6 dB by 80 kHz at 300 kHz, so read the spectrum for shape
and for differences between microphones rather than absolute level.

## `pdmraw <clk> <data> [rate <hz>]`

Opens a PDM data line **without** the PDM-to-PCM
filter, so the receiver delivers the raw bitstream. Nothing downstream decodes
it yet (`record`, `level`, `loopback` and `capture` refuse it); use
[`pdmbits`](#pdmbits-seconds--dump-frames) to see whether a microphone is responding. The
bit clock is 32 times `rate`, and `rate` is accepted up to 640 kHz (20 MHz),
well past what `pdm`'s filter can take.

**One line only.** The receiver captures just the first data line on this chip, however
many are listed: with two lines the data arrives as `[A, B, A', B']`, the first line's
pair twice, and a line listed second is ignored even with the slot mask and frame width
set as ESP-IDF documents. The command therefore refuses a list; use `pdm` with several
lines, which decimates in hardware, for more than two microphones.

## `pdmbench [frames [channels]]`

Times the software decimators and the capture statistics against each other
and cross-checks them, with no microphone. `channels` (2, 4, 6 or 8, default 2)
models that many microphones on a shared clock, so the cost of a larger array
can be measured before it is wired; the statistics are stereo and are run once
per channel pair and summed. It modulates two tones (10 and
50 kHz) into a synthetic 4.8 MHz PDM stream, then reports cycles per output
frame for the sinc^6 x16 decimator, the two-stage filter in plain C, and the
same filter on the S3's PIE vector unit; then cycles per frame for the capture
statistics as originally written (double-precision sum of squares), in 64-bit
integers, and on PIE. It states whether the PIE results match the plain ones
exactly. Tone levels are printed for the two decimators; the two-stage filter
is a different filter, so compare those, not samples.

## `pdmbits [seconds | dump [frames] | grab [seconds]]`

Reports statistics of the raw bitstream opened by `pdmraw`, with no decimation:
per slot, the share of ones, the share of adjacent bits that differ (`flips`),
and the standard deviation of the ones density over 1024-bit windows. A mic
that is clocked and alive sits near 50% ones with a high flip rate; a stuck or
sleeping part sits at a constant level or a regular pattern. Window-stdev rises
with sound, so it answers "does it respond" at clock rates the PDM-to-PCM
filter cannot take.

`pdmbits dump [frames]` instead captures up to 49152 frames into RAM (without
pausing for output, so the stream has no gaps) and prints one frame per line as
hex words, slot 0 then slot 1 for each data line, oldest first and most
significant bit first. It is for decimating and analysing the stream on a host.

`pdmbits grab [seconds]` is the same capture for longer than RAM allows, into
PSRAM (the S3's 8 MB; the build enables it in `sdkconfig.defaults.esp32s3`).
It keeps the words rather than printing them, up to 600 seconds, or as much as
PSRAM has free less 512 kB, which is 6.4 s of two microphones clocked at
4.8 MHz (1.2 MB/s) and 12.9 s at 2.4 MHz. A request that does not fit is
clamped and says so. It then reports how many frames per second the receiver
actually delivered against the nominal rate; a shortfall means the receiver
overran and the grab is missing data. Each grab replaces the last.

Fetch it over HTTP, which needs the device on a network (or its access point,
see [`wifi ap`](wifi.md)):

    ./tools/pdm_grab.py <host> grab.bin

`GET /api/capture` is behind the HTTP credentials. The file is the same
16-bit words as `dump`, little-endian, one frame after another; the channel
count and nominal frame rate come back as the `X-Channels` and
`X-Frames-Per-Second` headers, and `pdm_grab.py` prints them.

## `record [seconds]`

Captures the input and reports what was in it. Two seconds by default.

Both slots are always reported separately. A mono microphone fills one of them
and which one is a board fact, discovered the same way `audio tone ... left`
discovers which slot a mono amplifier plays.

```
> audio record 2
Captured 96000 frames at 48000 Hz from PDM microphone, 16-bit (full scale 32768)
slot           min         max      mean      stdev        rms      peak
left          -412         398      -1.8       58.3     -55.0 dB  -38.3 dB
right            0           0       0.0        0.0    -120.0 dB -120.0 dB
```

RMS is quoted about the mean, which is why the column is also labelled stdev. A
microphone with a DC offset is normal and says nothing about the signal;
counting that offset as level would make a silent part look like a loud one.

For scale, the Cardputer's SPM1423 in a quiet room sits between −62 and −70
dBFS and rises to about −33 dBFS when someone talks at it, on a DC offset of
roughly 1300 counts. A **stuck** input is called out explicitly rather than
left to be inferred, because it is the commonest way for one to be wrong and it
reads confusingly otherwise — RMS floors at −120 dB while peak sits near 0 dB,
peak being measured from zero for headroom and a dead line being nearly all
offset:

```
> audio record 1
slot           min         max      mean      stdev          rms       peak
left        -30935      -30935  -30935.0        0.0    -120.0 dB    -0.5 dB
The left slot never changed -- every one of 48000 samples read -30935. Nothing
is modulating this input: check that the part is clocked and that the data pin
is the right one.
```

Below the table is a spectrum in half-octave bands. Every FFT bin lands in
exactly one band, so a band is the *total power* in that half octave rather
than the level at its centre — which means a tone anywhere inside a band shows
up at its real level, and the bands sum to the broadband RMS above them. Both
of those are checked host-side; the first version of this probed each band at
one frequency and read a tone between two probes 89 dB low, which is the kind
of display that gets a working microphone thrown away.

## `capture [seconds]`

Writes the input to sequential files on an SD card at 48 kHz, rolling over to a
new file every 10 seconds.

Where [`record`](#record-seconds) reduces the input to statistics and a spectrum,
this keeps the samples. That is the difference worth knowing: `record` answers a
question here and now, and `capture` produces something to take away and open in
an editor, which is what you want once the question is "why does it sound like
that" rather than "is anything arriving".

It needs a card mounted at `/sd`, so [`sd spi`](sd.md) or [`sd mmc`](sd.md) comes
first; the mount is checked before the receiver is started, so a missing card is
reported rather than discovered part-way through a recording.

The rollover is not a convenience. A single file grown for minutes is one FAT
allocation chain to lose, and a card pulled mid-write takes the whole recording
with it; ten-second files lose ten seconds.

## `level [seconds]`

A meter rather than a measurement: one line per 100 ms with a bar per slot.
`record` answers "what is the input doing"; this answers "does it react to
me", which is the question you actually have while tapping a microphone, and
it needs the answer to arrive while your finger is still moving.

## `loopback [hz] [seconds] [level <pct>]`

Plays a tone and measures whether the input hears it. This is the end-to-end
test: output, air or wire, input, all in one command.

It measures the same frequency **twice** — once with the output silent, once
with it playing — and reports the difference. That control is the whole point.
A single measurement with the tone running cannot tell a microphone that hears
the speaker from one sitting next to a switching supply that happens to whine
near the test frequency; only the change between the two is evidence.

```
> audio loopback 1000
Testing whether the input hears 1000 Hz at 25% from the output.

slot          quiet    playing   change
left         -120.0     -120.0      0.0
right         -78.4      -13.1     65.3

Heard it: 1000 Hz rose 65.3 dB in the right slot when the output started.
```

A rise of 12 dB or more counts as heard, 6 to 12 dB as marginal, and less than
that as not heard. A negative result comes with the checks to make in the order
they are cheapest to answer.

Both a transmitter and a receiver have to be up, and on some boards that is
impossible — see the Cardputer below.

## `volume [pct]` and `mute [on|off]`

Delegated to the attached codec. A part with no volume control says so and
points at the generator's own `level` instead, which is a digital attenuation
and always available.

## `close`

Detaches the codec and releases I2S. The codec goes first: an amplifier still
enabled when its clocks stop thumps the speaker.

## NAU8822

Reached as `audio-nau8822 ...`. Drives the Nuvoton NAU8822 stereo codec with
speaker driver — the only part here that both plays and records, and so the
only one that can run `audio loopback` through real air rather than through the
chip's internal loopback. Its speaker driver and its ADC are on the same I2S
bus with no shared pin, which the Cardputer's microphone and speaker are not.

Control is I2C at `0x1a` (CSB low) or `0x1b` (CSB high), so
**the `i2c` menu's bus must be up first** — run `i2c bus <scl> <sda>` before
`audio-nau8822 init`. Audio arrives over I2S, so `audio bus` must be up too,
and it must have an `mclk` pin: the part is a slave and has no way to make a
system clock from BCLK alone. Without it every register write succeeds and
nothing comes out, so `init` refuses rather than leaving you to find that.

Registers are 7 address bits and 9 data bits packed into two bytes. The driver
keeps a **shadow copy** of everything it writes. That is not an optimisation:
several registers pack unrelated fields, and the family this part is
register-compatible with — the WM8978 — is write-only, so without a shadow
there would be no way to change one field without destroying its neighbours.

Identity is handled the same careful way `sht4x serial` is. The device ID
register is read first; if it answers, the part is identified outright. If it
does not, an acknowledged write is the only remaining evidence, and both `init`
and `status` say which of the two happened rather than implying more than was
established.

### `init [address]`

Resets the part, powers up the reference, bias and I/O buffer, configures the
audio interface and clocking for the current `audio bus` format, sets the DAC
to full scale, routes only the DAC into the output mixers — so a tone that
comes out came from I2S and nowhere else — enables thermal shutdown on the
speaker driver, and brings up both outputs.

The capture side comes up configured but **unpowered and disconnected**. See
`input` below for why that is not an oversight.

Volume is carried by the analog output attenuator, not the digital DAC volume:
attenuating digitally throws away bits, and the question on a bench is usually
whether the analog path works at all. 100 % is 0 dB; the six steps of gain
above that are deliberately not reachable.

### `input <mic|line|off> [boost]` and `gain [db]`

The capture side. This is the one part here that records *and* plays, so it
occupies both codec slots on its own and `audio record` works through it once
an input is selected.

**`init` deliberately leaves the ADC off.** The part has two input paths and
they are not interchangeable — a microphone goes in differentially on
MICP/MICN through the input mixer and the PGA, wanting mic bias and usually the
+20 dB boost; a line signal goes in on L2/R2 straight to the boost mixer,
skipping the PGA, wanting neither. Which is fitted is a board fact the driver
cannot read, and guessing wrong is not harmless: mic bias on a line output is
pointless at best, and 20 dB of gain on one clips instantly. So it waits to be
told.

`gain` follows whichever path is selected, because they are different
amplifiers with different ranges: the microphone PGA covers −12 to +35.25 dB in
0.75 dB steps, and the line input −15 to +3 dB in steps of 3. Both quantise, so
the value reported back is the one actually programmed rather than the one
asked for. `boost` is the separate +20 dB stage ahead of the ADC and only
exists on the microphone path — asking for it on the line input is refused
rather than ignored, since silently dropping it would leave you believing in
gain that is not there.

The ADC's high-pass filter is left on. An electret or MEMS input carries a DC
offset that is not signal, and the part can remove it in hardware — which is
the same problem `audio record` works around in software by quoting RMS about
the mean.

Selecting an input warns if the I2S bus has no receive line, because the ADC
then has nowhere to send samples and `audio record` would report a dead input
on a codec that is working perfectly. The full setup is `audio bus <bclk> <ws>
<dout> din <pin> mclk <pin>`, with `din` on the codec's ADCOUT.

**How this was verified without a microphone.** With nothing connected to the
inputs the ADC still sees its own front-end noise, and if the PGA is genuinely
in circuit then raising its gain has to raise that noise by the same number of
decibels. It does: over the PGA's full 47.25 dB the measured floor rose 48.0 dB,
and the +20 dB boost stage added 19.5 dB. At the bottom of the range the rise
lags slightly — 5.0 dB for a commanded 6 — which is the converter's own floor
showing through underneath, exactly as it should. The control is the same one
that confirmed the Cardputer's microphone: powered down, the ADC returns not a
small number but *exactly* zero.

The line path can only be half-checked this way. Its gain sits after the point
where the ADC's noise enters, so with no source connected the floor barely
moves; the registers demonstrably take effect, but the dB scale needs a real
signal to confirm.

### `status`, `reg <n> [value]`, `route <hp|speaker|both>`

`reg` reads or writes any of the 64 registers, showing both the shadow value
and the live one where the part reads back, and flagging a difference between
them. `route` chooses which output drivers are powered.

## ES8311

Reached as `audio-es8311 ...`. Drives the Everest ES8311 mono codec: a DAC for a
speaker amplifier and an ADC for an analog (or PDM) microphone, on one I2S bus. Like the
NAU8822 it both plays and records, so `audio loopback` works; unlike it, the part is
**mono** and **needs no MCLK pin**. It is what the M5Stack Cardputer Adv carries — see
`board-cardputer-adv` in [board](board.md).

Control is I2C at `0x18` (CE low) or `0x19` (CE high), so the `i2c` bus must be up first
— `i2c bus <scl> <sda>`, which the board preset does — and `audio bus` must be up too.

**Clocking without MCLK.** The part runs its converters from 256 × the sample rate. With
no MCLK pin it takes the bit clock instead, and 16-bit stereo at 8 kHz has a bit clock of
only 256 kHz; the codec's pre-multiplier brings that to 2.048 MHz. `init` works the
divider out from the clock the bus is actually running, so `audio bus ... rate 8000
bits 16` followed by `init` is the whole procedure. If the bus has an `mclk` pin the part
uses that instead. A clock no divider can reach is refused with the numbers, rather than
leaving a codec that accepts every write and plays at the wrong pitch. Rates are
8000–48000 Hz.

**Mono.** The wire is still two slots wide; the codec plays the left slot. `audio tone`'s
`right` argument is therefore silent here, and `both` or `left` is what to use.

### `init [address]`

Identifies the part from its chip ID registers, resets it, programs the clock and word
length for the current `audio bus` format, powers the analog blocks and the DAC, and sets
up the analog microphone. The DAC comes up **unmuted at 10 %**, deliberately not at the
last setting the previous owner left in the part.

Run `init` again after `audio bus` changes the format. `audio bus` reconfigures an
attached codec itself, but the reset it implies means any register written by hand with
`reg` is lost.

Volume maps percent onto decibels rather than register codes: 1 % is −50 dB, 100 % is
0 dB, 0 % is muted. The register itself spans 95.5 dB in half-dB steps, which as a linear
percentage would put 5 % at −90 dB — silence. The gain above 0 dB the part can apply is
deliberately not reachable from `audio volume`.

### `status` and `reg <n> [value]`

`status` shows the I2C address, volume as percent, register code and decibels, mute, the
clock source and the divide and multiply in use, and the microphone path.

`reg` reads or writes one 8-bit register (`0x00`–`0xff`); the chip ID is at `0xfd`,
`0xfe` and `0xff`. Unlike the NAU8822 the registers read back, so there is no shadow copy
and the value shown is what the part holds.

### `gain [db]`

The analog microphone preamp, 0 to 30 dB in 3 dB steps; with no argument it shows the
current setting. It starts at 18 dB.

## NS4168

Reached as `audio-ns4168 ...`. A mono I2S class-D amplifier with **no control
bus at all** — it takes BCLK, LRCK and SDATA and drives a speaker, and
everything configurable about it is set by resistors on the board. It is the
useful counterexample for the codec interface: if a part with one pin and no
registers fits the same abstraction as a codec with sixty, the abstraction is
at the right level. It implements mute and nothing else, and `audio volume`
says plainly that there is no volume to set.

### `init [sd <pin>]`

Attaches the amplifier and enables it. The I2S clocks must already be running:
an amplifier enabled into a dead bus latches onto whatever the lines happen to
be doing, and the ESP32 leaves them low.

`sd` is the shutdown pin. Give it and `audio mute` works; omit it and the part
is assumed hard-enabled, which is the case on boards that do not break it out —
the Cardputer is one.

Note that on many designs **the SD pin doubles as channel select** through a
resistor divider, so which slot the amplifier plays is a board decision rather
than a software one. `audio tone 1000 3 left` followed by the same with `right`
is the quick way to find out which. On the Cardputer the answer is **right**:
a left-only tone is silent there, and that is the board, not a fault.

### `status`

Shows the enable pin and its state. It also says outright that nothing here
confirms the part really is an NS4168 — with no control bus, there is no way to
ask.

## SPH0645

Reached as `audio-sph0645 ...`. The Knowles SPH0645LM4H-B I2S MEMS microphone,
and the input-side counterpart to the NS4168: also no control bus, also no
registers. So why does it get a driver at all, when `audio bus ... din <pin>`
already receives I2S?

Because the datasheet imposes three constraints that are invisible from the
command line and that fail *quietly* rather than loudly:

- **The oversampling ratio is fixed at 64**, so WS must be BCLK/64. Two 32-bit
  slots give exactly that. Ask for 16-bit slots and the frame is 32 clocks, the
  part sees word-select at twice the rate it expects, and what comes back is
  not silence but plausible-looking rubbish.
- **The clock must be 2.048–4.096 MHz**, which at 64 clocks per frame means a
  sample rate of 32–64 kHz and nothing else. Below 900 kHz the part
  deliberately sleeps and tri-states its data pin, which reads as a dead
  microphone rather than as a misconfiguration.
- **SELECT decides which slot it lands in** — low drives the line while
  word-select is low, which in I2S is the left slot. It is usually strapped on
  the board, so the software cannot read it and has to be told.

`init [left|right] [sel <pin>]` checks the first two against the live bus and
records the third. Give `sel` a GPIO and it drives it, which turns "which slot
is my microphone in" from a question into an experiment you can run twice.

`status` reports the slot, the measured bit clock against the datasheet range,
and what the part guarantees: 24-bit words with 18 bits of real precision and
the low bits always zero — worth stating, because it means zeros down there are
the part working to specification rather than something truncating in this
firmware. It also gives the sensitivity (94 dB SPL reads −26 dBFS) so there is
something to compare a recording against, and notes that a single microphone on
a bus needs a 100 kΩ pull-down on the data line: without it, the half of the
frame the part tri-states floats to whatever the bus capacitance holds, and the
other slot mirrors the first instead of reading silence.

**Not verified against a real part.** There is no SPH0645 on the bench, so this
is transcribed from the Rev B datasheet. The 32-bit capture path it depends on
*has* been exercised on hardware through the internal loopback, and so have all
of its refusals; the part itself has not.

Other 24/32-bit I2S microphones — the INMP441, the ICS-43434 — work through the
same `audio bus ... din <pin> bits 32` and the same `audio record`, without
this submenu. They differ in their clock limits, so the checks here are the
SPH0645's rather than universal.

## USB audio

`audio-usb` presents the open microphone to a computer as a USB audio device, so
`arecord`, Audacity or a script can record it live with no custom transport. It
works on chips with a USB device controller (ESP32-S2, S3, P4); on others every
command here says so.

**Starting it ends the serial console.** The USB device and USB-Serial-JTAG share
the chip's one USB PHY, so after `audio-usb start` the serial port is gone until
the next reset. Everything else still works over the web console and MQTT. To
flash again, reset the board (or replug it, holding BOOT if it does not
re-enumerate).

**What fits is a property of the chip.** A USB audio packet must fit whole in the
endpoint's transmit FIFO, and the S3's controller has one 1 KB block of FIFO RAM
shared by every FIFO. After the receive FIFO and endpoint 0 that leaves 712 bytes
per 1 ms packet for the whole stream, whatever the channel count — less than the
1023 bytes the USB full-speed protocol allows. A packet carries `floor(rate/1000)
+ 1` frames, so the stream needs `(floor(rate/1000) + 1) × channels × bytes`
bytes, and `config` refuses what does not fit:

| Rate | 16-bit, most channels on an S3 |
|---|---|
| 300 kHz | 1 |
| 192 kHz | 1 |
| 150 kHz | 2 |
| 100 kHz | 3 |
| 75 kHz | 4 |
| 48 kHz | 7 |
| 32 kHz | 8 |

24-bit costs half again as much. Two ultrasonic microphones at 300 kHz need 1.2
MB/s, which no full-speed device can carry; take one channel at 300 kHz, or both at
150 kHz or less. The P4 and S31 have high-speed USB and a larger FIFO; the check
uses the controller the firmware is running on.

**The rate is the microphone's.** Nothing here resamples. To change the rate, reopen
the microphone at the rate you want (`audio pdm ... rate <hz>`, `audio pdmultra ...
maxhz <hz>`); `config` checks that it fits and refuses a `rate` the microphone is not
running at. Close nothing the stream depends on while it runs: run `audio-usb stop`
before `audio close` or reopening the microphone. If you do, the stream ends and
`status` says the microphone was closed under it.

**One stream at a time.** `audio-usb` and [`audio-net`](#network-audio) read the same
receiver, and two readers would each get every other block, so the second to start is
refused and told who has the microphone.

**Three kinds of microphone can be streamed.**

- A decimated PDM microphone (`audio pdm`, `audio pdmultra`): one PDM line, two
  channels, at the rate the receiver produces.
- An I2S microphone (`audio bus ... din`): the slots the bus carries, 16, 24 or 32
  bits.
- An undecimated single-line PDM receiver (`audio pdmraw`, `audio pdmultra ... raw`):
  one line, two microphones. The stream decimates it in software with the sixth-order
  sinc filter, and the rate is the bit clock divided by 64, 48, 32 or 16 — `config
  rate` picks one, and 64 (the standard PDM oversampling) is the default. `audio
  pdmraw 10 11 rate 96000` runs a 3.072 MHz clock, so 48 kHz by default.

- **Several lines go through `audio pdm`**, not `pdmraw`: `audio pdm 10 11,12 rate
  32000` is four channels decimated by the chip, so it costs the processor nothing,
  and its rate is the clock divided by 64. A standard-mode microphone wants 1 to 2.4
  MHz at start-up, which is 16 to 37 kHz here; a clock above that needs the part's
  ultrasonic entry sequence: `audio pdm … rate 48000..75000 ultra` performs it for
several lines, and `pdmultra` for one line with a software filter.

Each line carries a left and a right microphone, and the order the receiver delivers
them is not the same on the raw path as on the filtered one: the same two microphones
came out swapped. Tap a microphone and look at which channel moves before trusting the
labels.

On Linux the device appears as a USB microphone:

```sh
arecord -l                                  # find the card
arecord -D hw:CARD=<name>,DEV=0 -c 2 -r 48000 -f S16_LE -d 10 mic.wav
```

Use the channel count and rate `config` printed.

### `config [rate <hz>] [ch <list>] [bits <16|24>]`

Chooses the stream and checks it against the bandwidth, without starting USB. `rate`
must be one the microphone offers (see above). `ch`
takes a comma-separated list of the microphone's channels (`0,1`, or `1` for the
second microphone alone); it defaults to all of them. `bits` is 16 by default, and
24 needs a 24- or 32-bit source. Every call states the whole configuration: anything
left out goes back to following the microphone.

When it does not fit, the error says what would: the most channels at this rate, and
the highest rate at this channel count.

### `start [timeout <seconds>]`

Checks the stream again against the microphone as it is now, warns that the serial
console is about to end, starts the USB device and begins streaming. Samples are
dropped (and counted) until the host opens the device.

`timeout` resets the chip after that many seconds, which is what brings the serial
console back without touching the board: the way to run a timed recording on the
bench. Without it the stream runs until `stop` over the web console, or a reset.

### `status`

Whether the host has connected and is recording, the stream and packet size against
the budget, bytes sent and dropped, buffer overruns, how full the host-side buffer
is, and — when the microphone is decimated in software — the decimator's cost in
cycles per frame and as a share of one core.

### `stop`

Stops the stream and the USB device. The serial console comes back after a reset, not
before.

### `budget`

Lists, for common rates and the open microphone's, the most channels a USB packet holds
at 16 and 24 bits on this chip. Needs no microphone.

## Network audio

`audio-net` streams the open microphone over WiFi as a WAV file with no end, which
`ffplay`, VLC, sox, a script or a browser's `<audio>` element plays as it arrives. It
works on every chip, leaves the serial console alone, and needs no cable.

There is no `start`: the listener's request starts the stream, and the microphone is held
for as long as it stays connected. `config` only sets what a listener gets when it does
not ask for anything particular.

```sh
curl -su <user>:<password> http://<device>/api/audio/stream | ffplay -nodisp -
curl -su <user>:<password> http://<device>/api/audio/stream | sox -t wav - take.wav trim 0 10
curl -su <user>:<password> 'http://<device>/api/audio/stream?ch=1&bits=24' > right.wav
```

The user and password are the `http` section's of the configuration. The route needs them
because it is whatever the microphones hear. `rate`, `bits` and `ch` in the query mean what
they do in `audio-net config`, and override it for that listener.

**One listener.** A second request is refused with `409` and the name of what has the
microphone; so is one that arrives just after a listener left, while its buffer is still
closing (try again). A listener that disconnects frees the microphone at once.

**The buffer is what makes WiFi usable.** The receiver overruns if it is not read every few
milliseconds, and a socket send stalls for as long as WiFi does. So the stream writes into a
buffer and a second task empties it into the socket: when WiFi stalls the buffer fills, and
when it is full whole blocks are dropped and counted. A stall costs a gap in the recording,
never an overrun in the receiver.

- **On a board with PSRAM the buffer lives there**, and a second of 4-channel 48 kHz audio
  (384 kB) costs nothing that matters. `buffer <ms>` sets how much to hold, from 20 ms to 10 s
  (the default is one second).
- **Without PSRAM it is internal RAM**, shared with WiFi, so the buffer is capped to leave
  40 kB free; `config` says when the one you asked for does not fit and what you get instead.
  The ESP32-C3 has room for a few hundred milliseconds of speech-rate audio and little more.
- **A bigger buffer is also more delay.** After a stall a live listener hears the backlog, not
  the present. Hold only as much as the stalls you want to ride out.

**What the radio can carry.** Measured on an ESP32-S3 over 802.11n at 20 MHz: four
microphones at 75 kHz (4.8 Mbit/s) streamed at 100% with nothing dropped, and a link that
`wifi iperf` measured at 5 Mbit/s with a weak signal carried about 7.5 Mbit/s of stream when
the signal was strong. Stereo at 300 kHz needs 9.6 Mbit/s, which this link does not carry.
Throughput moves a lot with signal, so measure yours with [`wifi iperf`](wifi.md) and let
`status` tell you: it counts what was dropped, and a stall of a second or two does happen.
The firmware raises lwIP's TCP send window to 32 kB for this (`sdkconfig.defaults`); at its
default of 5760 bytes a stream could not exceed about 3.7 Mbit/s whatever the link could do.
`wifi.power_save` (off unless the configuration turns it on) puts the radio to sleep between
beacons, which makes a stream stutter; leave it off. A board whose radio will not join at full
power (the Sinn) needs `wifi txpower 8` first, and that lowers the signal the stream sees.

The rate is the microphone's, as for USB: nothing is resampled, and a request for another one
is refused with the rate it runs at.

### `audio-net config [rate <hz>] [ch <list>] [bits <16|24>] [buffer <ms>]`

Chooses what a listener gets by default, checks it against the open microphone, and prints the
bit rate it will use, the buffer and where it lives. `rate`, `ch` and `bits` are as for
[`audio-usb config`](#config-rate-hz-ch-list-bits-1624). Every call states the whole
configuration: anything left out goes back to following the microphone, and the buffer to one
second. Nothing starts; the first request does.

### `audio-net status`

Whether a listener is connected and who, the format, how long it has been connected, bytes sent
and the average bit rate, the buffer's fill and its high-water mark in milliseconds (how close
the stream came to dropping), what was dropped and how many read errors there were, and, for a
microphone decimated in software, the decimator's cost. After a listener leaves, the same
numbers stay, with the reason it ended: it was stopped, the listener went away, or the
microphone was closed or stopped delivering.

### `audio-net stop`

Disconnects the listener and releases the microphone. The listener sees the response end after
the audio still in the buffer.
