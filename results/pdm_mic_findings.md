# SPH0641LU4H-1 PDM microphones on an ESP32-S3: findings

As of 2026-10-15. Raw per-run numbers are in `pdm_mic_levels.md`; plots are
`pdm_ultrasonic_response.png` and `pdm_outdoor_vs_indoor.png`.

## Setup

ESP32-S3 DevKitC, two PDM mics on one data line (L/R by SEL pin).
**CLK = GPIO8, DAT = GPIO3** (GPIO18/GPIO8 gave a stuck line: GPIO18 showed
bare-pin capacitance, i.e. nothing connected). Datasheet: Knowles rev B,
sensitivity -26 dBFS at 94 dB SPL, SNR 64.3 dB(A), ultrasonic mode
3.072-4.8 MHz, must not be powered up or woken into ultrasonic mode.

## Working microphones (standard mode, 3.072 MHz, 48 kHz)

| set | what | L rms | R rms | L-R |
|-----|------|-------|-------|-----|
| A | original pair | -58.5 | -58.9 | 0.4 dB |
| B | replacement pair, same model | -56.7 | -56.8 | 0.1 dB |
| C | single mic, other model | -67.2 (one slot, both slots identical) | | DC -241 counts |
| D | one A + one B | -57.8 | -49.9 | 7.9 dB |
| F | both A, re-seated | -58.6 | -58.3 | 0.3 dB |
| G | one mic swapped from F | -58.6 | -58.9 | 0.4 dB |

- Matched pairs agree within 0.5 dB. Room noise drifts ~1.5 dB between runs,
  so compare L-R within a set, not absolute level across sets.
- Set C is ~10 dB quieter with a large DC offset: a different part, not a fault.
- Set D: one set B mic read ~8 dB hot with asymmetric excursions and DC -7 to
  -12. Not isolated to a specific mic: set G (after a swap) was clean.
  Re-seating cured set E's identical-L/R fault, which was a contact or SEL
  problem.

## Sample-rate and clock limits on the S3

- Hardware PDM-to-PCM filter divides by 64: usable to **104 kHz PCM
  (6.66 MHz clock)**; 112-128 kHz break (clock out of the part's range); 120
  reads zero; 192 kHz is refused by the driver. Not enough for 80 kHz of
  bandwidth at an in-spec clock (4.8 MHz / 64 = 75 kHz).
- Raw (undecimated) mode works but is capped near 5 MHz by the driver
  (needs 16 x bit clock < 80 MHz). The mics respond: ~50% ones, 70% flips;
  a floating pin reads 100% ones.
- **Ultrasonic path that works:** `audio pdmultra`. Start at 2.048 MHz, wait
  100 ms, raise to 4.8 MHz on the same channel, decimate in software
  (sinc^6, x16, MSB-first) to 300 kHz PCM. Needs the CPU at **240 MHz** and
  `-O2` for the decimator and `capture.c`; at 160 MHz or -Og the receiver
  overruns silently and the floor reads ~20 dB too high.

## Ultrasonic signal characterization (4.8 MHz, 0.1-80 kHz)

Noise floor, median 5-80 kHz (dBFS/Hz, slot 0 / slot 1):

| location | floor | integrated 20-80 kHz (dBFS) |
|----------|-------|------------------------------|
| indoor  | -118.0 / -116.1 | -66.7 / -66.4 |
| outdoor | -116.6 / -115.3 | -68.8 / -67.8 |

- The floor is flat from ~4 to 60 kHz (about 5 dB of ripple), the same on both
  slots and independent of mic clock (4.8 / 4.0 / 3.2 MHz): it is electrical
  noise, not acoustic.
- **Indoor spurs** at 15.6, 22.5, 28, 31.3, 54.5 and 62.6 kHz (62.6 kHz is
  +21 to +24 dB). They did not move with the mic clock, so they are not PDM
  products. **Outdoors they are all gone.** They come from the indoor
  environment (acoustic or radiated pickup); which was not separated.
- Audible band: outdoor 0.1-1 kHz -68.7/-68.2, 1-8 kHz -72.0/-71.4,
  8-20 kHz -72.6/-70.8 dBFS: ambient/wind dominated. Slots agree within 1 dB.
- L-R coherence outdoors 0.64 (0.1-1 kHz), 0.46 (1-8 kHz), ~0.2 above 8 kHz:
  shared sound at low frequency, independent mic noise at ultrasonic.

## Against the datasheet

- Datasheet response (ultrasonic mode, acoustic, re 1 kHz): +14.6 dB peak at
  25 kHz, about +8 dB from 40 to 65 kHz, +12.8 dB at 80 kHz.
- **Not verified.** No ultrasonic source was available, so only the noise
  floor was measured, and it is electrical. No bump at 25 kHz is expected
  from it and none is seen.
- Datasheet in-band noise implies about -90 dBFS A-weighted (20 Hz-8 kHz).
  The flat floor extrapolates to roughly -79 dBFS unweighted over the same
  band: perhaps 8 dB above spec, but both rooms add acoustic noise there, so
  this is an upper bound, not a mic failure.

## Corrections

- The first ultrasonic results (-50 dBFS RMS, floor rising ~1.3 dB per
  half-octave to 64 kHz) were receiver overruns, not the part. They are marked
  superseded in `pdm_mic_levels.md`.
- Sinc droop (~6 dB at 80 kHz for sinc^6 x16) is not compensated on-device.

## Still open

1. Acoustic response: needs a known ultrasonic source (40 kHz transducer).
2. Which set B mic was hot in set D.
3. Whether the indoor spurs are acoustic or electrical (cover the ports).
4. S3 default CPU frequency is still 160 MHz in `sdkconfig.defaults.esp32s3`.
