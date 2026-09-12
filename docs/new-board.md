# Bringing up a new board

[← Command reference](README.md) · [Project README](../README.md)

A running order for the first hour with a board nobody has powered before. The
goal is narrow: **find assembly and design errors quickly**, on a PCB with
several chips on it, before anyone writes a BSP or trusts a single line of
application code.

The order below is not arbitrary. Each stage produces the facts the next one
assumes, and skipping ahead is how bringup goes wrong — a codec that will not
initialize is indistinguishable from a codec whose SDA and SCL are swapped, and
you can spend a day on the driver either way. So:

1. **[Pins first](#stage-1--survey-the-pins)**, with nothing else driving them.
   Wiring, shorts, pull-ups, straps.
2. **[Then the buses](#stage-2--find-the-i2c-bus-and-check-it-against-the-schematic)**.
   Does anything answer, and is it what the schematic promised?
3. **[Then the parts](#stage-3--exercise-each-part-in-turn)**, one at a time,
   each with a stimulus and a control.
4. **[Then write it down](#stage-4--record-what-you-learned)** so the second
   board takes ten minutes.

Throughout, prefer a measurement that *changes when you change one thing* over a
measurement that merely looks plausible. Most of the commands here are built
around that idea, and the ones that are say so in their own output.

## Stage 0 — Power, flash, and the chip you think you have

Before the console exists, the usual: current draw at the bench supply against
what the design predicts, and rails measured at the load rather than at the
regulator. A board that browns out under WiFi load will produce confusing
results at every stage below, and it will look like a peripheral fault.

Then flash this firmware — `idf.py set-target`, `build`, `flash monitor`, as in
the [project README](../README.md#building-and-running) — and run
[`sys info`](system.md#info). It answers three questions worth answering
before any peripheral work:

- **Is it the part that was ordered?** Chip model, silicon revision, core count
  and flash size come from the silicon, not from the BOM.
- **How did it start?** A reset reason of brownout, or a board that reboots
  every few seconds, means everything after this point is measuring a power
  problem.
- **What identifies this board?** The station MAC is the one genuinely unique
  number, and it is what stamps the [SD results file](sd.md#the-results-file).

If the design fits a 32.768 kHz crystal, [`sys lfxtal`](system.md#lfxtal) is
a two-second check that it actually oscillates — it measures the clock rather
than asking the chip which source is selected, which is the only way to tell a
missing crystal from a fitted one.

**Get onto the web console early.** The device joins a network or raises its own
access point at boot without being asked, so a board with no network to join is
already hosting one — SSID and passphrase come from `ap_ssid` and `ap_password`
in `config.json`, at `http://192.168.4.1/`. Both interfaces share the same output
fan-out, so you can drive the board from a browser while your hands are on a
scope probe, and the serial log still records everything. See
[net](net.md) for the configured link and [wifi](wifi.md) for the radio.

## Stage 1 — Survey the pins

**Nothing may be driving the pins during this stage.** Unplug the daughterboard,
lift the probe, and do not run these against a bus another device shares.

### Start with [`gpio survey`](gpio.md#survey-pin)

The first command to run on a board you do not know. It measures every pin the
chip may safely drive and classifies each one:

- **Pulled up, a few kΩ** — a deliberate fitted resistor. Two adjacent pins like
  this are almost always I2C, and the survey says so and suggests the `i2c bus`
  command to try next. This is the one thing it can genuinely infer.
- **Driven low or high** — something on the board is holding the pin: a strap
  resistor, a card-detect switch, an output on another chip, an enable tied off.
- **Weak pull-up or floating** — nothing fitted. The capacitance column
  separates a bare pad (a couple of pF) from a routed net (more), and *that is
  all it does*.
- **Skipped, with the reason** — flash, PSRAM, a peripheral currently holding
  the pin, or the console. Sweeping the console pins would cut off the
  connection the results are printed to.

**Do not read more into it than that.** A clock or data line is driven, not
pulled, and from inside the chip an idle I2S line looks like any other idle
input. A pin-permutation search built on the opposite assumption is
[the one thing deliberately deleted from `tools/`](../tools/README.md#what-is-deliberately-not-here);
the lesson was kept and the script was not.

If the board might already be known, [`board list`](board.md#list) and
[`board <name> pins`](board.md#board-pins) will tell you, and save the rest of
this stage.

### Hunt for shorts with [`gpio short`](gpio.md#short-pin)

Run it over each connector or each package edge — `gpio short 38-44` for an SD
slot, a header's worth of pins for a daughterboard. It drives each pin low in
turn and watches the others.

**Adjacent pins are tested and reported first**, because a solder bridge at the
package is far and away the most common way two nets become one, and
neighbouring GPIO numbers are usually neighbouring pads. A short is only
reported when both pins pull each other down; a pin held low by the board
follows everything and fights nothing, so those are listed separately as
untestable rather than reported as shorted to the world.

This is the highest-value command in the whole document for a freshly assembled
board. The worked example in its own docs is a bridge between SD D0 and SCK
that presented as *the host never starting its clock* — a symptom that points
nowhere near a solder bridge.

### Measure the passives with [`gpio rc`](gpio.md#rc-pin-ref-pin-kohms)

`short` asks whether two nets are the same net. `rc` asks whether a net is
pulled up, how hard, and how much is hanging off it — which a logic read cannot,
because a 10 kΩ pull-up to a healthy rail and a 10 kΩ pull-up whose far end is
floating both read as `1`.

What it catches on a new board:

- **A pull-up that was never fitted**, or fitted at the wrong value. Compare
  against the schematic value.
- **A rail that is not there.** A net whose pull-up goes to an unpowered
  regulator reads as tens of nF of decoupling capacitance rather than the
  single-digit pF of a signal net. That is a factor of a thousand, and survives
  the calibration uncertainty easily.
- **A device that is present.** An SD card in the slot shows DAT3 at roughly
  half its neighbours, because of the card's own internal detection pull-up.

Calibrate with `ref` against a net with a known fitted resistor and nothing else
on it — the internal pull-up is nominally 45 kΩ, measured 35 kΩ on the S3 here,
and every absolute number scales with it. **Ratios between pins are exact
regardless**, so read the table as a comparison first and as absolute values
second.

### Confirm the rest by hand

- **Straps, buttons, card-detect, jumpers:** [`gpio read <pin> up`](gpio.md).
  The internal pull-up is what makes this meaningful — without it, a pin driven
  low and a pin connected to nothing both read 0. Then *change something* and
  read again. A button that reads 0 proves nothing; a button that goes 1 → 0
  when pressed proves the whole path.
- **Outputs, LEDs, enables:** [`gpio set`](gpio.md#set-pin-state), or
  [`gpio blink`](gpio.md#blink-pin-count-period_ms) when you want to watch it
  from across the bench.
- **Anything analog** — divider on a battery, a thermistor, a rail brought out
  to a test pin: [`gpio aread`](gpio.md#aread-pin), which reports both the raw
  count and, where the chip has calibration in eFuse, millivolts.
- **Backlights, buzzers, fans:** [`gpio-pwm set`](gpio.md#set-pin-freq-duty).
  The frequency actually achieved is reported, since the timer divider
  quantizes it.

At the end of this stage you should be able to draw the board's pinout from
measurement, and you should know about every solder bridge on it.

## Stage 2 — Find the I2C bus and check it against the schematic

I2C comes before everything else because it is the cheapest bus to prove and
because half the parts on a typical board sit on it — and because several other
subsystems here, the NAU8822 codec among them, need it working before they can
be touched at all.

### Open the bus

[`i2c bus <scl> <sda>`](i2c.md#bus-scl-sda), using the pins `gpio survey`
flagged as pulled up. **Try both orders.** The survey cannot tell SCL from SDA —
they look identical from inside the chip — so it suggests both, and swapping
them is one command. A wrong order fails to scan rather than misbehaving
subtly.

### Write down the expected addresses first, then [`i2c scan`](i2c.md#scan)

The scan is not the test. **The comparison against the schematic is the test**,
and doing it in that order stops you rationalizing whatever appears. List every
part on the bus and the address each one's strapping should produce, then scan
and reconcile. Three outcomes, each meaning something different:

| Result | What it usually is |
|---|---|
| Expected, present | Power, pull-ups, addressing and routing all correct for that part. |
| Expected, **missing** | Rail or enable pin down, part not populated, address straps wrong, reset held asserted, or a bridge on its net. |
| **Unexpected**, present | An address strap floating or fitted to the wrong side — very common on parts whose address pins encode several bits. |
| Nothing at all | Bus not pulled up, pins swapped, or a line held low. The scan reports the state of the bus in that case rather than an empty table. |
| *Every* address answers | SDA stuck low, or SDA and SCL shorted together. Go back to [`gpio short`](gpio.md#short-pin). |

The addressing failures are worth expecting specifically, because they look like
missing parts: an [INA237](i2c.md#ina237) occupies anywhere in `0x40`–`0x4f`
depending on how A0/A1 are strapped, and an [SHT4x](i2c.md#sht4x) is `0x44`,
`0x45` or `0x46` purely by part variant. A device answering one address off from
the schematic is a strap resistor, not a dead chip.

### An ACK is not an identification

Something is at that address; the scan does not say *what*. Before you believe
the bus, ask a part to prove it is the part:

- [`i2c-ina237 config <addr>`](i2c.md#config-address-shunt_ohms) reads
  `MANUFACTURER_ID` and refuses anything that is not `0x5449`.
- [`i2c-sht4x serial`](i2c.md#serial-address) returns a 32-bit serial with
  per-word CRC-8 — on a part with no ID register, a serial that checksums is the
  available evidence.
- [`i2c-nau7802 status`](i2c.md#status) reads the revision register and the
  power/gain/rate registers back out of silicon, so it reports how the part is
  *actually* configured rather than what this firmware believes it did.
- Anything else: [`i2c read <address> <bytes>`](i2c.md#read-address-bytes1)
  against a known ID register.

## Stage 3 — Exercise each part in turn

Now the individual hardware, in whatever order the board makes convenient.
Cheapest first is a good default: a sensor is one command, an SD slot is a
handful, an audio path is an afternoon.

### Sensors on I2C

Identify, read, then **stimulate and watch the reading move** — that last step
is what separates a working sensor from a register map full of plausible
defaults.

- [`i2c-sht4x read`](i2c.md#read-address-highmediumlow) for temperature and
  humidity; [`heater`](i2c.md#heater-address-mw-ms) is the stimulus, and a
  wildly out-of-range humidity is reported uncropped on purpose, because during
  bringup that is a signal rather than noise.
- [`i2c-ina237 config`](i2c.md#config-address-shunt_ohms) with the board's
  actual shunt value, then [`read`](i2c.md#read-address). Switch a load on and
  the current should follow. `DIAG_ALRT` and `SHUNT_CAL` are checked on every
  read, so a part that reset since configuration is flagged rather than
  reporting mis-scaled current.
- [`i2c-nau7802`](i2c.md#nau7802) for a load cell: `init` (with `ldo 3.0` if the
  breakout relies on the internal regulator — see the warning there about
  putting two sources on one net), `gain 128`, `tare`, `calibrate`, `weight`. A
  reading pinned at full scale is reported as saturation, which during bringup
  usually means the bridge is disconnected, unexcited or miswired.

### Load cells on an HX711

[`loadcell`](loadcell.md) is the other load cell front end, and it is a
top-level menu rather than an `i2c` submenu because the HX711 has no bus at all
-- two bit-banged pins are the whole interface. `init <dout> <sck>`, then
`gain`, `tare`, `calibrate`, `weight`, exactly as above.

Its `init` is the part worth knowing during bringup. A part with no ID register
cannot be asked to identify itself, so it proves the two wires separately: DOUT
must fall (only something driving the line can do that, since the pin is pulled
up), and DOUT must come back high after the clock burst (only the clock reaching
the device can do that). It then refuses if five conversions come back
bit-identical, because a live bridge always dithers and an exactly repeating
value -- `0` most of all -- is what a pin not connected to the part looks like.

Then do to it what settles any pin question: **move the clock**. Re-running
`init` with PD_SCK on a pin wired to nothing, DOUT unchanged, must report that
DOUT never came back high — a part that still answered would mean the reading
was never coming from the pin you thought. See
[HardwareSupport.md](../HardwareSupport.md) for what has been confirmed.

### Capacitive touch pads

Identify the pads, then stimulate and watch the reading move — same rule as
the I2C sensors above, applied to a whole array at once rather than one part.
[`touch watch <pads>`](touch.md#watch-pads-seconds) calibrates a baseline for
every pad given and then reports **all of them, every tick**, which is what
makes adjacent-pad interference visible: touch one pad and watch whether its
neighbours' numbers move too, not just whether the one you pressed crosses its
threshold. ESP32-S3 only — the ESP32-C3 has no touch peripheral, and the
command says so rather than doing nothing.

### SD card

Check the pull-ups on the bus with [`gpio rc`](gpio.md#rc-pin-ref-pin-kohms)
before anything else — the sample table in that section is an SD slot, and it
shows what right looks like, including CLK correctly having *no* pull-up and DET
held low by a seated card.

Then, in this order:

1. **[`sd mmc`](sd.md#mmc-clk-cmd-d0-d1-d2-d3-khz-freq) before
   [`sd spi`](sd.md#spi-clk-mosi-miso-cs-khz-freq)**, if the board wires the SD
   host at all. A card put into SPI mode stays in SPI mode until its power is
   removed — a board reset does not undo it — so testing SD mode first saves a
   power cycle. Three pins select 1-bit, six select 4-bit.
2. **[`sd info`](sd.md#info)** for identity: card type, CID, capacity, negotiated
   bus width, and *two* clocks — what the host is really clocking at, and what
   the card advertises. Together they say whether the interface or the card is
   the limit.
3. **[`sd bench`](sd.md#bench-size_kb-block_kb)** and
   [`sd raw`](sd.md#raw-size_kb-block_kb-start_sector) for throughput. Read the
   *worst single block* as well as the average; a card that stalls 800 ms for
   internal housekeeping will decide whether a logging design works, and an
   average hides it completely.
4. **[`sd sweep`](sd.md#sweep-max_khz-size_kb-block_kb)** when you want signal
   integrity rather than speed. It is read-only and CRC-verified against a
   reference read at 20 MHz, because an overclocked card usually does not fail —
   it succeeds and hands back corrupt data. On a new layout, the rate at which
   data stops matching is a statement about the routing.

Failures here are mostly stage-1 problems in disguise. Init that hangs with the
clock never starting is the classic bridged-pads signature; a card that answers
at 400 kHz and not at speed is wiring or pull-ups; and note that the 20 MHz SPI
ceiling documented in [that page](sd.md#reported-numbers) is a software
threshold in ESP-IDF rather than anything about your board.

### Audio

The longest chain on most boards, and the one that fails silently most often, so
build it up in pieces that each prove something.

1. **[`audio bus <bclk> <ws> <dout>`](audio.md)** — the transmitter starts
   immediately and sends silence, so BCLK and WS are live and can be put on a
   scope before any signal exists. Clocks are reported as *configured*, not as
   requested.
2. **Internal loopback: set `din` to the same pin as `dout`.** The driver feeds
   the transmitter straight back to the receiver through the pad, with nothing
   connected. This is the one capture test that needs no hardware at all, which
   makes it the right first move when a microphone reads silent — it separates a
   broken receive path from a broken part.
3. **[`audio tone 1000 3`](audio.md#tone-hz-secondscontinuous-level-pct-leftrightboth)**
   and read the *measured* sample rate, not the configured one. A shortfall
   means something starved the DMA. Try `left` and then `right`: on a mono
   amplifier, which slot it plays is a board fact — often set by a resistor
   divider on the shutdown pin — and this is how you discover it.
4. **The part itself.** [`audio-ns4168 init`](audio.md#ns4168) for a bare
   amplifier (clocks must already be running, or it latches onto whatever the
   idle lines are doing). [`audio-nau8822 init`](audio.md#nau8822) needs
   [`i2c bus`](i2c.md#bus-scl-sda) up *and* an `mclk` pin — without MCLK every
   register write succeeds and nothing comes out, so `init` refuses rather than
   leaving you to find that. Its capture side comes up deliberately off; tell it
   `input mic` or `input line`, because guessing wrong puts mic bias on a line
   output or 20 dB of gain into something that clips instantly.
5. **Microphones.** [`audio pdm <clk> <data>`](audio.md#pdm-clk-data-rate-hz)
   for a PDM part — it reports the microphone clock and warns when the rate puts
   it outside the 1–3.25 MHz a MEMS part typically needs, below which it sleeps
   and looks exactly like a dead microphone. An I2S microphone works through
   `audio bus ... din <pin> bits 32`; the [SPH0645](audio.md#sph0645) submenu
   adds that part's three quiet-failure constraints.
6. **[`audio level`](audio.md#level-seconds)** while tapping the microphone —
   the question you actually have is "does it react to me", and the answer needs
   to arrive while your finger is still moving. Then
   [`audio record`](audio.md#record-seconds) for the numbers: both slots
   reported separately, RMS quoted about the mean, a half-octave spectrum, and
   an explicit call-out when an input never changed at all.
7. **[`audio loopback`](audio.md#loopback-hz-seconds-level-pct)** as the
   end-to-end test. It measures the same frequency twice — output silent, then
   playing — and reports the difference, because only the change is evidence. A
   single measurement cannot distinguish a microphone that hears the speaker
   from one sitting next to a switching supply that whines near the test tone.

**Watch for shared pins.** On the Cardputer the speaker's word-select and the
microphone's clock are the same GPIO, so that board physically cannot record its
own speaker; the commands refuse rather than letting the second peripheral
quietly steal the pad from the first. Check your own pinout for the same trap
before blaming a part. And note `audio pdm` is unavailable on the ESP32-C3,
which can clock a PDM microphone but has no filter to decode it.

### Displays

1. **[`display bus <clk> <mosi> <cs> <dc> ...`](display.md#bus-clk-mosi-cs-dc-rst-pin-bl-pin-miso-pin-khz-n)**
   then **[`display-st7789 init <width> <height>`](display.md#init-width-height-gap-x-y-bgr-invert-onoff)**
   — a successful init draws [`edges`](display.md#edges) immediately, so a
   blank screen at this point is the bus or the panel, not yet the pattern.
2. **[`display bars`](display.md#bars)** for colour order: red and blue
   swapped means add `bgr` to `init`; white and black ends swapped means
   `invert off` (or `on`, if the board needed the other default).
3. **[`display edges`](display.md#edges)** for orientation and offset — the
   tool this firmware has for exactly that question. A missing coloured side
   means [`display gap`](display.md#gap-x-y) is off on that axis; noise on one
   side means the gap is too large. Work out the four rotations with
   [`display orient`](display.md#orient-swap_xy-mirror_x-mirror_y).
4. **[`display grid`](display.md#grid-step)** to confirm the whole area scans
   correctly once the above look right — dead rows or columns and edge
   clipping show up here that a border alone would not.

If the picture is noisy rather than simply wrong, lower `khz` on
[`display bus`](display.md#bus-clk-mosi-cs-dc-rst-pin-bl-pin-miso-pin-khz-n)
before suspecting the panel.

### SPI, UART, and the radio

- **[`spi bus <clk> <mosi> <miso> [cs]`](spi.md#bus-clk-mosi-miso-cs)** then
  [`spi read`](spi.md#read-addr-len) against a known ID register. Give it the
  chip-select pin — without one the driver cannot select anything and reads
  return bus noise that looks like data. The SPI host is shared with the `sd`
  menu; [`spi free`](spi.md#free) hands it over.
- **[`uart init <tx> <rx> <baud>`](uart.md#init-tx-rx-baud)** is an auxiliary
  port, never the console, so it cannot take over the shell you are typing into.
  A jumper from TX to RX plus [`send`](uart.md#send-data) and
  [`receive`](uart.md#receive) proves both pins and the level shifter in one
  move, before you blame the modem on the other end.
- **[`wifi scan`](wifi.md#scan)** exercises the RF path — antenna, matching
  network, and the connector nobody reflowed. Compare RSSI against a known-good
  board on the same bench.
  [`wifi iperf`](wifi.md#iperf-serverport) then loads the radio properly, which
  is as much a power-integrity test as a throughput one: a board that resets or
  browns out here has a supply problem that nothing in stage 1 would show.
  Both `wifi status` and `wifi iperf` report the negotiated PHY mode and
  channel bandwidth alongside RSSI: a link stuck on `802.11b` or `20 MHz` when
  the AP is capable of `n`/`40 MHz` points at RF (distance, interference, AP
  configuration) as the cause of low throughput. A healthy PHY/RSSI with
  throughput still below expectations points elsewhere — check
  [`wifi netstats`](wifi.md#netstats) before and after the run: a rising
  `drop` or `err` count confirms packet loss or buffer pressure in the
  network stack rather than the radio. This board's `sdkconfig` currently runs
  IDF's stock TCP window (`CONFIG_LWIP_TCP_WND_DEFAULT=5760`) and a small
  AMPDU block-ack window (`CONFIG_ESP_WIFI_TX_BA_WIN=6`), both common
  throughput ceilings on a link that otherwise looks fine — `netstats` is how
  to confirm that's actually what's happening rather than guess.

## Stage 4 — Record what you learned

- **Add the pinout to the firmware.** [`board`](board.md) holds known pinouts
  and per-subsystem presets, and every pin can carry a note saying whether it
  came from a schematic or from having actually been exercised. That distinction
  is the whole value of the table: "this is the clock pin" and "the schematic
  says so" are different claims.
- **Script the sequence.** [`tools/bringup.py`](../tools/README.md) drives the
  console over serial and is importable by tests. Every command reports failure
  as a line beginning with `ERR:`, so a script can tell success from failure
  without parsing prose — which is what makes a bringup checklist runnable
  against board number two through fifty.
- **Keep regression tests for the drivers you had to write.** The
  [`tools/hwtest/`](../tools/README.md#hwtest--runs-against-a-board) scripts read
  values back from the device rather than from the driver's shadow copy, so they
  test the part and the bus as well as the code — and most of them do not need
  the audio pins to be right, meaning a codec driver can be tested before the
  wiring is understood.
- **Lift any signal processing onto the host.**
  [`tools/hosttest/`](../tools/README.md#hosttest--runs-on-the-build-machine)
  compiles the audio maths with gcc and checks it analytically. It once caught a
  spectrum reading 89 dB low — a bug that on hardware looks precisely like a
  dead microphone, with all the evidence pointing at the part.
- **The SD results file** (`/sd/sdbench.txt`) already does this for storage: each
  entry is stamped with the chip, the firmware revision, the station MAC and the
  pins by role, so a card carried between boards accumulates a comparable
  history.

## Symptom → first command

| Symptom | Try this first |
|---|---|
| Board is unknown, no schematic to hand | [`gpio survey`](gpio.md#survey-pin) |
| Peripheral init hangs with no error | [`gpio short`](gpio.md#short-pin) over its pins |
| Pin reads the right level but the part is dead | [`gpio rc`](gpio.md#rc-pin-ref-pin-kohms) — pull-up to an unpowered rail |
| `i2c scan` finds nothing | Swap SCL/SDA; check pull-ups with `gpio rc` |
| `i2c scan` finds everything | SDA stuck low or shorted to SCL |
| Device answers one address off | Address strap fitted to the wrong side |
| Device ACKs but data is garbage | Identify it properly — `sht4x serial`, `ina237 config` |
| SD card never clocks | Bridged pads on the data bus ([`gpio short`](gpio.md#short-pin)) |
| Touch pad always or never reads active | [`gpio rc`](gpio.md#rc-pin-ref-pin-kohms) to check the net, then [`touch watch`](touch.md#watch-pads-seconds) |
| Pressing one touch pad also triggers a neighbour | [`touch watch`](touch.md#watch-pads-seconds) with both pads listed — the peak-deflection summary quantifies it |
| SD works slow, fails fast | [`sd sweep`](sd.md#sweep-max_khz-size_kb-block_kb) — it verifies data, not just errors |
| Microphone records silence | `audio bus ... din <dout pin>` internal loopback |
| Microphone records one unchanging value | Wrong data pin, or the part is not clocked |
| Amplifier silent on one channel | [`audio tone 1000 3 left`](audio.md#tone-hz-secondscontinuous-level-pct-leftrightboth), then `right` |
| Codec configures but is silent | No MCLK, or the input path was never selected |
| Board resets under load | [`wifi iperf`](wifi.md#iperf-serverport) and a scope on the rail |
| Display stays blank | [`display bus`](display.md#bus-clk-mosi-cs-dc-rst-pin-bl-pin-miso-pin-khz-n) then `init` — `edges` draws on a successful init, so blank after that means backlight or the `bl` pin |
| Display colours swapped | [`display bars`](display.md#bars) — red/blue swapped needs `bgr`, black/white ends swapped needs `invert` |
| Display image shifted or noise band | [`display edges`](display.md#edges) then [`gap`](display.md#gap-x-y); noise band specifically means lower `khz` on `display bus` |

## Two habits worth keeping

**A reading that looks reasonable is not evidence; a reading that changes when
you change one thing is.** Every stage above is built that way — the pull-up
measured against a known reference, the tone measured twice with the output
silent and playing, the gain raised to see whether the noise floor follows.

**Prefer a refusal to a plausible silence.** Most of the commands here fail
loudly on purpose: `audio-nau8822 init` refuses without MCLK, `sd sweep` refuses
to derive a limit from a card that cannot reproduce its own data, `i2c-nau7802
calibrate` refuses when the reading has not moved clear of the noise. Anything
you add during a bringup should do the same, because the alternative is a number
that looks fine and is not.
