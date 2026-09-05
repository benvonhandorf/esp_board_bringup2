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

The signal is generated from a 1024-entry sine table with interpolation rather
than by calling `sinf()`. The S3 has an FPU and would manage; the C3 has none,
and softfloat there would spend a large part of a core generating the test
signal, which is precisely when the question is whether the *hardware* keeps
up. The table's error floor measures 90 dB below the signal, far under anything
an amplifier under test contributes. Phase is a full 32-bit accumulator, so no
frequency has to divide into the buffer length and block boundaries are
inaudible, and every tone gets a 5 ms raised-cosine fade at each end so it does
not thump the speaker.

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
Frequency steps once per block of 256 frames — 5 ms at 48 kHz — with phase
carried across, so there is no clicking at the steps.

## `stop`

Ends a continuous tone, ramping it down rather than cutting it.

## `pdm <clk> <data> [rate <hz>]`

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
