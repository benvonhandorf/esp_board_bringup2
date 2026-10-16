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
- **Partly verified.** The noise floor is electrical (above). A 44 kHz
  transducer is detected cleanly in the 45 kHz band (see the 44 kHz section),
  but with one frequency and half-octave bands the response shape and the
  25 kHz peak are still unchecked.
- Datasheet in-band noise implies about -90 dBFS A-weighted (20 Hz-8 kHz).
  The flat floor extrapolates to roughly -79 dBFS unweighted over the same
  band: perhaps 8 dB above spec, but both rooms add acoustic noise there, so
  this is an upper bound, not a mic failure.

## CPU cost of `audio pdmultra` (measured, 240 MHz, one core)

| maxhz | PCM rate | decimator | whole capture task | PCM bandwidth |
|-------|----------|-----------|--------------------|---------------|
| 80 kHz | 300 kHz | 300 cycles/frame, 32.5% | 96% | 1.2 MB/s |
| 40 kHz | 150 kHz | 530 cycles/frame, 28.7% | 68% | 0.6 MB/s |
| 20 kHz | 75 kHz  | 994 cycles/frame, 26.9% | 54% | 0.3 MB/s |

Raw input is 1.2 MB/s (9.6 Mbit/s) in every case. The decimator costs roughly
250-300 cycles per raw stereo frame; the rest of the capture task is the
per-sample statistics and the read copy. At 80 kHz the task is at 96% of one
core, so there is almost no headroom: the earlier estimate of 25-30% was low.

## Optimized decimation and statistics (`pdmultra ... fast`, `pdmbench`)

Synthetic-input benchmark (`audio pdmbench`, 240 MHz, cycles per 300 kHz output
frame):

| item | before | after | factor |
|------|--------|-------|--------|
| decimator: sinc^6 x16 | 300 | - | - |
| decimator: two-stage (sinc^4 bytes + 24-tap FIR), plain C | - | 383 | 0.8x (slower) |
| decimator: two-stage, PIE dot product | - | 159 | 1.9x |
| statistics: original (double sum of squares) | 454 | - | - |
| statistics: int64 | - | 94 | 4.8x |
| statistics: PIE | - | 11 | 41x |

PIE results are bit-identical to the plain-C ones (decimator and statistics).
The two-stage filter is a different filter: 10 kHz tone level matches the
sinc^6 one (-12.1 dBFS), the 50 kHz tone is 1.9 dB higher because it droops
less. On real microphone data (host-side) the two floors agree within about
2 dB from 4 to 80 kHz.

On the device, real capture, `audio record 2`, one core at 240 MHz:

| configuration | decimator | whole capture task |
|---------------|-----------|--------------------|
| before (sinc^6, original statistics) | 32.5% | 96% |
| PIE statistics only | 32.5% | 49% |
| PIE statistics + two-stage filter (`fast`) | 17.4% | 33.9% |

Most of the saving was the statistics: they were summing squares in
double-precision software floating point. The two-stage filter is worth about
15 points of one core. Stage 1 is still table lookups, so SIMD cannot help
there; that is what bounds the decimator at about 160 cycles per frame.

## Channel scaling (`audio pdmbench 1024 <channels>`, 240 MHz, 300 kHz PCM)

Decimator cycles per frame, and share of one core at 300 kHz in the bench
(the bench's cold-cache figures run about 15% above the on-device ones):

| channels | sinc6 x16 | two-stage PIE | statistics PIE | PIE output matches plain C |
|----------|-----------|---------------|----------------|----------------------------|
| 2 | 302 (37.7%) | 160 (20.0%) | 14 (1.7%) | yes |
| 4 | 534 (66.7%) | 319 (39.8%) | 23 (2.9%) | yes |
| 6 | 766 (95.7%) | 477 (59.6%) | 34 (4.2%) | yes |
| 8 | 998 (124.7%) | 635 (79.4%) | 44 (5.5%) | yes |

The two-stage decimator is linear in channels (about 80 cycles per channel);
sinc^6 has a fixed part and costs about 116 per channel. Adding the measured
2-channel overhead of the capture task (I2S read copy and bookkeeping, about
15 points, assumed to scale with bandwidth), a single capture task would be
about 34% (2 ch), 68% (4 ch), 100% (6 ch) and 135% (8 ch) of one core at
240 MHz with the default filter. Six or more channels need the work split
across both cores. Whether the I2S driver delivers more than two raw slots
correctly is unverified.

## On-device half-octave measurements: ports covered and 44 kHz source (2026-10-15)

All from `audio pdmultra 8 3` then `audio record 5` (300 kHz PCM, two-stage
decimator, 4.8 MHz clock). Values are power per half-octave band in dBFS as
`record` prints them, so they are band totals, not dBFS/Hz, and are not
comparable with the floor figures above. Resolution is a half octave, so a tone
lands in a band, not at a frequency.

### Ports covered (acoustic or electrical?)

| band | open L / R | covered L / R |
|------|------------|---------------|
| overall rms | -64.4 / -66.7 | -63.3 / -63.7 |
| 62 Hz | -63.4 / -70.5 | -55.0 / -60.3 |
| 16 kHz | -83.2 / -76.6 | -87.7 / -96.3 |
| 22.6 kHz | -81.7 / -79.2 | -92.8 / -92.7 |
| 32 kHz | -84.1 / -79.1 | -84.4 / -83.4 |
| 45 kHz | -79.4 / -79.2 | -84.9 / -83.1 |
| 64 kHz | -64.3 / -70.7 | -76.9 / -77.6 |
| 90 kHz | -80.9 / -80.3 | -79.3 / -80.4 |

- The 16 and 22.6 kHz spurs fall 11-20 dB when the ports are covered: they are
  largely **acoustic**.
- The 64 kHz spur falls 7-12 dB but is still the highest ultrasonic band (3-8 dB
  above 32 and 45 kHz): partly acoustic, partly electrical or conducted.
- 62 Hz **rises** 9-10 dB when covered: electrical (hum or supply ripple), not
  acoustic.
- The covered 32-45 kHz level, about -83 to -85 dBFS per band, is the best
  estimate of the electrical floor. Above 80 kHz the figure (about -80) is the
  filter and decimator floor.
- rms is not lower covered because the 62 Hz hum dominates it.

### 44 kHz transducer

| run | 45 kHz L / R | rms L / R | peak L / R | note |
|-----|--------------|-----------|------------|------|
| close | -2.9 / +0.8 | -6.0 / -2.2 | -2.5 / 0.0 | R clipped; skirts are clipping distortion |
| moved away | -16.9 / -5.8 | -18.9 / -11.3 | -14.0 / -5.4 | clean |
| same again | -24.9 / -11.1 | -28.0 / -14.8 | -21.1 / -10.1 | position drifted |
| rig | -16.7 / -6.3 | -18.0 / -9.1 | -11.5 / -2.6 | clean; reproduces "moved away" within 0.5 dB |

- The tone is 66 dB (L) and 77 dB (R) above the covered-port floor in the clean
  runs, with no overrun symptoms (decimator 18.9% of one core).
- The right slot reads about 10 dB hotter than the left in every position-stable
  run. The mics matched within 0.5 dB in standard mode, so this is geometry
  (angle or distance to the transducer), not gain.
- In the clean runs the only other elevated band is 64 kHz (-58 / -66 dBFS,
  roughly 60 dB below the tone), a harmonic or skirt; 32 and 90 kHz are at the
  floor on L and 5-7 dB above it on R.
- Plots: `pdm_44khz_transducer_response.png` (third row) and
  `pdm_44khz_rig_response.png` (rig); dashed line is the covered-port left
  slot.

## Corrections

- The first ultrasonic results (-50 dBFS RMS, floor rising ~1.3 dB per
  half-octave to 64 kHz) were receiver overruns, not the part. They are marked
  superseded in `pdm_mic_levels.md`.
- Sinc droop (~6 dB at 80 kHz for sinc^6 x16) is not compensated on-device.

## Still open

1. Acoustic response across frequency: a 44 kHz source is detected; a swept or
   multi-frequency source and a finer spectrum than half-octave bands are needed.
2. Which set B mic was hot in set D.
3. Which indoor spurs are electrical: 16 and 22.6 kHz are acoustic, 62 Hz is
   electrical, 64 kHz is mixed. 15.6, 28, 31.3, 54.5 kHz not resolved by
   half-octave bands.
4. S3 default CPU frequency is still 160 MHz in `sdkconfig.defaults.esp32s3`.
