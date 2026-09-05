

## Audio Codecs

Support outputting a simple tone or a frequency sweep over a set time period.

**Implemented** as the `audio` group — see [docs/audio.md](docs/audio.md). Codecs are
their own groups, implementing the `audio_codec_t` vtable in `main/audio/audio.h`; adding
a part is a new file, a row in the registry in `audio.c`, and a group in
`app_console.c`.

### NAU8822

Implemented: `audio-nau8822`. Control over I2C at 0x1a/0x1b, audio over I2S,
requires MCLK. Both directions: playback through the DAC and speaker driver,
capture through the ADC via `audio-nau8822 input <mic|line|off>` with analog
gain on `audio-nau8822 gain`. The only part here that does both, and therefore
the only one that can run `audio loopback` acoustically.

Verified on hardware (2026-08-13) except the DAC and the analog outputs, which
need a transducer or a loopback jumper to observe. Confirmed: identity and the
full 64-register map against the mainline reset defaults, the init sequence,
every input-path register write, format encoding across seven rate/width
combinations, output volume, mute and routing, and — the part that matters —
the ADC producing real data whose noise floor tracks the input PGA to within
0.75 dB over 47 dB of range.

### NS4168

Implemented: `audio-ns4168`. No control bus; optional shutdown pin.

## Microphones

Support both mono and stereo setups.
Sample the microphone for several seconds and report min, max, stdev of the audio data as well as an ASCII frequency plot for simple debugging and to ensure audio data is being received.

**Implemented** as `audio record`, `audio level` and `audio loopback` — see
[docs/audio.md](docs/audio.md). Both slots are always reported separately, so a mono
part's slot is discovered rather than assumed. `record` gives min, max, mean,
stdev, RMS and peak plus a half-octave power spectrum; `loopback` plays a tone
and measures whether the input hears it, against a silent control.

### PDM Microphones

e.g. SPM1423

Implemented: `audio pdm <clk> <data>`. No control interface exists on these
parts, so there is no driver and no group — the transport is the whole of it.
Requires a hardware PDM-to-PCM filter, which the ESP32-C3 lacks.

### I2S Microphones

e.g. Knowles SPH0645LM4H-B I2S Microphone

Implemented: `audio bus <bclk> <ws> <dout> din <pin> bits 32` receives from any
of them, and `audio-sph0645` adds the Knowles part's own constraints — a fixed
oversampling ratio of 64 that forces 32-bit slots, a 2.048–4.096 MHz clock
limit that forces a 32–64 kHz sample rate, and a SELECT strap that decides
which slot it lands in. Each of those fails silently rather than loudly, which
is what earns the part a driver despite having no control bus.

Untested against real hardware; there is no SPH0645 on the bench. The 32-bit
capture path and every refusal have been exercised on the Cardputer through the
internal loopback.

Other 24/32-bit I2S microphones (INMP441, ICS-43434) work through the bus
command and `audio record` without a group.

### Loopback

`audio loopback` needs a transmitter and a receiver at once, which is not
always possible: on the M5Stack Cardputer the microphone clock and the
speaker's word-select are the same GPIO, so that board cannot record its own
speaker. `audio bus ... din <dout pin>` is the fallback that always works — the
I2S driver loops the transmitter back internally, which tests the capture path
without testing anything outside the chip.

## Capacitive Touch Pads

Take a set of pads, calibrate a baseline for each, then report every pad's
live reading (not just on/off) so adjacent pads' crosstalk is visible, not
just whichever pad crossed its own threshold.

**Implemented** as `touch watch <pads> [seconds]` — see
[docs/touch.md](docs/touch.md). Uses the ESP32-S3's native touch sensor
peripheral (hardware version 2, GPIO1–14, channel number equals GPIO number);
compiled out with a clear refusal on the ESP32-C3, which has no touch
peripheral at all. Distinct from the FT6236 below: this is the SoC's own
GPIO-pad sensing, not an external I2C touchscreen controller.

Untested against real hardware; there is no touch pad hardware on the bench.
Builds warning-clean for both `esp32s3` and `esp32c3`.

## Load Cells

Bridge front ends: bring the converter up, tare it, calibrate against a known
mass, then report weight in those units.

### NAU7802

Implemented: `i2c-nau7802` — see [docs/i2c.md](docs/i2c.md#nau7802). I2C at a
fixed 0x2a, with an optional DRDY line on `init drdy <pin>`.

Verified on hardware. Confirmed: the power-up sequence, internal offset
calibration, gain and rate encodings, the `CTRL2.CHS` bit position (bit 7, not
bit 0 — a wrong bit there fails without any error at all), and the tare/
calibrate/weight path — the last re-confirmed 2026-08-26 against a 2 kg cell
with a 100 g mass, giving 976.2 counts per gram at gain 128 (97,621 counts for
100 g, 1.16% of full scale) and a scale good to ±1.10% over 50 samples.

That calibration was impossible before the same date: the noise guard tested
peak-to-peak spread rather than the uncertainty of the mean, and at ~4800
counts RMS it demanded 241,900 counts against a 97,621-count signal — and
demanded *more* the harder you averaged. See the note in
[docs/i2c.md](docs/i2c.md#read-samples-tare-samples-calibrate-known-mass-samples-weight-samples). Its noise floor on the sensor board is ~3400 counts RMS
at 10 SPS and nothing reachable from firmware moves it; the ruled-out list is
in [AGENTS.md](AGENTS.md).

### HX711

Implemented as the top-level `loadcell` group — see
[docs/loadcell.md](docs/loadcell.md). No control bus: DOUT and PD_SCK are
bit-banged, and the number of trailing clock pulses is the whole configuration
surface (25 = channel A gain 128, 26 = B/32, 27 = A/64).

Verified on hardware (2026-08-26) against an HX711 on the ESP32-C3 sensor
board, DOUT on GPIO 10 and PD_SCK on GPIO 3, with a bridge attached. The
datasheet carries no revision number, so it is cited by URL and retrieval date
in the file header rather than by revision. Builds warning-clean for both
`esp32c3` and `esp32s3`.

**Confirmed:** the clock burst and 24-bit read, sign extension (steady
−103,500 counts at gain 128, spread ~150), the pulse-to-gain mapping *and its
direction* — 25 pulses reads 1.91× what 27 does against an ideal 2.00, the
shortfall being a gain-independent offset of ~5100 counts — channel switching,
the settling discards, power down and up with the mode re-applied afterwards,
`close`, the measured rate (11.6–12.3 SPS, so the RATE strap is low), and the
calibration noise guard refusing with no mass on the cell. Worst PD_SCK high
phase observed was 1 µs against the datasheet's 50 µs limit, so the IRAM burst
holds comfortably.

**The wiring checks were confirmed by making them fire**, which is the part
worth having: unconnected pins time out on DOUT, a console pin is refused as
PD_SCK, and — the useful one — moving *only* the clock to a pin wired to
nothing produced `DOUT did not return high after the clock burst` rather than a
plausible number. That is the two-wires-proved-separately design doing its job,
and it is the same "move the pin and watch it break" test that settled the
Cardputer microphone.

The tare/calibrate/weight path is confirmed against a 2 kg cell with a 100 g
mass (2026-08-26): 1108.7 counts per gram at gain 128, 110,873 counts for
100 g, scale good to ±0.01% over 50 samples, and an empty cell reading
0.026 ±0.008 g afterwards.

**Still unconfirmed:** the 80 SPS strap (this board is strapped to 10), and the
stretched-pulse and saturation/stuck-value refusals, which never fired because
nothing provoked them — a live bridge dithers and the timing held.

## Displays

For displays, run a test pattern over he displays showing different colors and drawing a grid over the display area.
Provide tooling to help determine offsets and orientation settings.

### ILI9488 IPS Display

### ST7789

### ST7789V2 - M5Stack Cardputer

## Input Accessories

### FT6236 Touch controller

Report when touch data is detected and X/Y coordinates of touches

### M5Stack Cardputer Keyboard Matrix



---

## Verified in this build

The port onto `idf_template` and the shared components was exercised against the
**ESP32-C3 sensor board** (INA237 ×2 per the silkscreen, NAU7802, SHT4x, relay,
microSD), 4 MB flash, USB-Serial-JTAG console on `/dev/ttyACM0`, on 2026-09-05.
Everything above this line was verified against the pre-port firmware and carries
over unchanged; what follows is what this hardware could actually answer.

| Group | Result |
|---|---|
| `sys info`, `sys status` | ESP32-C3 rev v0.3, 4096 KB flash, MAC 7c:df:a1:a3:99:40 |
| `sys lfxtal` | **32.768 kHz crystal fitted and running**: measured 32767 Hz, RTC slow clock switched to XTAL32K |
| `net status`, `net scan` | manager state and address reported |
| `wifi scan` | all three visible APs, with BSSID, channel and security |
| `wifi netstats` | lwIP counters read back |
| `gpio read`, `gpio survey` | 22 pins censused; pull-ups on 4, 5, 11; GPIO 7 held low |
| `gpio-pwm` | registered; not exercised against a load |
| `i2c bus`, `i2c scan` | 0x2a, 0x44, 0x46 found on SCL 5 / SDA 4 |
| `i2c-sht4x` | **0x44**: serial 0x0E8BB7AF, 27.2 °C / 41 %RH |
| `i2c-ina237` | **0x46**: bus 4.32 V, −13.8 mA, die 26.5 °C |
| `i2c-nau7802` | **0x2a**: revision 0x0F, powers up, offset calibration passes at gain x128 |
| `i2c-ina219`, `i2c-ina226` | both correctly *refuse* 0x46 — see below |
| `i2c-lm75bdp`, `i2c-rx8130ce`, `i2c-aw9523b`, `i2c-pi4ioe` | absent parts reported as absent |
| `spi`, `sd` | bus comes up; the two correctly refuse each other the SPI host |
| `audio` | I2S on 6/9/10 at 48 kHz; 1 kHz tone played, rate measured back at 48000 |
| `loadcell` | registered; no HX711 fitted, refuses with the command that is missing |
| `board-sensor` | `pins` and the `i2c` preset both run |
| `uart`, `touch` | registered; `touch` stubs out on the C3, which has no touch peripheral |

### The part at 0x46 is an INA237, and the firmware can prove it

Three current-monitor groups were pointed at the same address:

- `i2c-ina237 read 0x46` succeeds and returns plausible values.
- `i2c-ina226 read 0x46` refuses: *manufacturer 0x5449, die 0x2381; expected
  0x5449 and 0x2260*. TI, but the INA237's die.
- `i2c-ina219 read 0x46` refuses on the calibration read-back, which is the only
  identification that part offers.

Two independent refusals agreeing with the third succeeding is the identification
working, and it is the reason all three parts have groups rather than one
standing in for the others.

### Not verified

- **The NAU7802's analog path.** It powers up and calibrates, but raw reads peg
  at negative full scale — an open bridge input, which is the board's state, not
  a firmware result. Tare, calibrate and weight are unexercised here.
- **The second INA237.** The silkscreen says two; `i2c scan` finds one. Either
  the second is unpopulated on this unit or it is at an address outside
  0x40–0x4f.
- **The sensor board's SD slot.** The pinout in `main/board/board.c` lists GPIO
  38–44, which do not exist on an ESP32-C3 (0–21). Those rows look copied from
  the minstro entry and should be treated as unknown until someone reads the
  schematic; there is deliberately no `board-sensor sd` preset.
- **Audio beyond the transport.** The C3 has I2S and the bus, tone and rate
  measurement all work, but no codec is fitted, so `audio-nau8822`,
  `audio-ns4168`, `audio-sph0645`, `record`, `level`, `loopback` and `capture`
  are build-verified only.
- **OTA.** `tools/flash_a.py` was used throughout; an over-the-air round trip has
  not been run against this build.
