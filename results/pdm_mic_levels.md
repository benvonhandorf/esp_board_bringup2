# PDM microphone levels

Setup: ESP32-S3 DevKitC, `audio pdm 8 3` (CLK=GPIO8, DAT=GPIO3), 48 kHz,
3.072 MHz mic clock, `audio record` (96000 frames, 2 s). Moderately noisy room.
RMS is dBFS (16-bit, full scale 32768); stdev is in raw counts.

## Set A: original two mics, 2026-10-15

| run | L rms | R rms | L stdev | R stdev | L-R rms |
|-----|-------|-------|---------|---------|---------|
| 1   | -58.6 | -59.0 | 38.5    | 36.9    | 0.4 dB  |
| 2   | -57.6 | -58.3 | 43.2    | 39.9    | 0.7 dB  |
| 3   | -58.6 | -59.0 | 38.4    | 36.7    | 0.4 dB  |
| 4   | -59.1 | -59.4 | 36.4    | 35.0    | 0.3 dB  |
| mean| -58.5 | -58.9 | 38.6    | 37.1    | 0.4 dB  |

DC offset about -1 to -3 counts on both. Left is consistently ~0.4 dB hotter.

Add new sets below, same setup, same room, same mic positions.

## Set B: two replacement mics (same model), 2026-10-15

| run | L rms | R rms | L stdev | R stdev | L-R rms |
|-----|-------|-------|---------|---------|---------|
| 1   | -57.6 | -57.9 | 43.1    | 41.6    | 0.3 dB  |
| 2   | -57.0 | -56.9 | 46.5    | 46.7    | 0.1 dB  |
| 3   | -56.3 | -56.3 | 50.1    | 49.9    | 0.0 dB  |
| 4   | -56.0 | -56.1 | 52.1    | 51.2    | 0.1 dB  |
| mean| -56.7 | -56.8 | 48.0    | 47.4    | 0.1 dB  |

Levels rose ~1.6 dB over the four runs on both channels together (ambient
noise drift), so compare L-R within a set, not absolute level across sets.

## Set C: single mic, different model, 2026-10-15

One mic only. L and R are sample-for-sample identical (it drives both slots,
or the second slot mirrors the first), so there is one channel of data.

| run | rms (L=R) | stdev | min  | max  | mean   |
|-----|-----------|-------|------|------|--------|
| 1   | -68.4     | 12.5  | -298 | -173 | -241.5 |
| 2   | -66.9     | 14.8  | -337 | -165 | -241.4 |
| 3   | -66.9     | 14.9  | -307 | -177 | -241.6 |
| 4   | -66.4     | 15.7  | -320 | -159 | -241.6 |
| mean| -67.2     | 14.5  |      |      | -241.5 |

About 10 dB below sets A/B (-56.7 to -58.5) with a DC offset of -241 counts
(A/B: about -1). rms is AC only, so the offset does not inflate it.

## Set D: one mic from A + one from B, 2026-10-15

Which physical mic is on L vs R is not recorded (slot follows each mic's SEL).

| run | L rms | R rms | L stdev | R stdev | L peak | R peak | R min |
|-----|-------|-------|---------|---------|--------|--------|-------|
| 1   | -58.9 | -49.7 | 37.4    | 107.5   | -43.8  | -23.3  | -2245 |
| 2   | -57.1 | -50.0 | 45.5    | 103.8   | -44.0  | -24.1 | -2042 |
| 3   | -57.6 | -50.2 | 43.3    | 100.8   | -44.6  | -23.8 | -2122 |
| 4   | -57.4 | -49.6 | 44.2    | 108.3   | -42.4  | -24.1 | -2036 |
| mean| -57.8 | -49.9 | 42.6    | 105.1   |        |        |       |

R is ~7.9 dB hotter than L, vs 0.1-0.4 dB within sets A and B. R shows large
asymmetric negative excursions (min ~-2100 vs max ~+740) and DC -7 to -12.
L matches the A/B level (-56.7 to -58.5).

## Set E: both set A mics (re-test of the D outlier), 2026-10-15

| run | L rms | R rms | L stdev | R stdev | L peak | R peak | note |
|-----|-------|-------|---------|---------|--------|--------|------|
| 1   | -58.9 | -58.9 | 37.1    | 37.1    | -42.3  | -42.3  | L==R identical |
| 2   | -59.1 | -59.2 | 36.3    | 36.1    | -30.8  | -46.8  | transient on L |
| 3   | -58.9 | -59.0 | 37.1    | 36.9    | -29.6  | -46.8  | transient on L |
| 4   | -59.2 | -59.2 | 35.8    | 35.8    | -46.9  | -46.9  | L==R identical |
| mean| -59.0 | -59.1 | 36.6    | 36.5    |        |        |                |

Level matches set A (-58.5/-58.9) and L-R is 0.1 dB. The hot, asymmetric right
channel in set D therefore came from the set B mic that was in that pair.
Runs 1 and 4 have L and R sample-for-sample identical, which two independent
mics should not produce -- check the SEL pins (see set C).

## Set F: both set A mics, re-seated, 2026-10-15

| run | L rms | R rms | L stdev | R stdev | L-R rms |
|-----|-------|-------|---------|---------|---------|
| 1   | -58.2 | -58.1 | 40.1    | 41.0    | 0.1 dB  |
| 2   | -58.9 | -58.6 | 37.3    | 38.4    | 0.3 dB  |
| 3   | -58.4 | -58.1 | 39.2    | 40.6    | 0.3 dB  |
| 4   | -58.7 | -58.4 | 38.0    | 39.4    | 0.3 dB  |
| mean| -58.6 | -58.3 | 38.7    | 39.9    | 0.3 dB  |

L and R are distinct in every run (the identical-sample runs in set E did not
recur) and no transients. Right is consistently ~0.3 dB hotter here; set A had
left hotter by 0.4 dB, so the slot-to-mic assignment may have swapped.

## Set G: one mic swapped from set F, 2026-10-15

Which mic was swapped in is not recorded here.

| run | L rms | R rms | L stdev | R stdev | L-R rms |
|-----|-------|-------|---------|---------|---------|
| 1   | -58.6 | -59.0 | 38.4    | 36.9    | 0.4 dB  |
| 2   | -59.0 | -59.2 | 36.9    | 35.7    | 0.2 dB  |
| 3   | -59.2 | -59.5 | 35.9    | 34.7    | 0.3 dB  |
| 4   | -57.4 | -57.9 | 44.1    | 41.6    | 0.5 dB  |
| mean| -58.6 | -58.9 | 38.8    | 37.2    | 0.4 dB  |

No hot channel; matches sets A/B/F. Run 4 is ~1.5 dB up on both channels
together (room noise, not the mics).

## High sample rate / ultrasonic test, 2026-10-15 (current pair, set G)

`audio pdm 8 3 rate <hz>`; mic clock = 64 x rate. Spectrum extended to 24
half-octave bands (AUDIO_BANDS) so bands above 22.6 kHz are shown.

| rate    | mic clock | result |
|---------|-----------|--------|
| 48 kHz  | 3.07 MHz  | normal |
| 96 kHz  | 6.14 MHz  | normal, L/R -56.6/-57.1 dBFS |
| 104 kHz | 6.66 MHz  | normal, L/R -56.7/-57.2 dBFS; highest usable |
| 112 kHz | 7.17 MHz  | broken: -20/-34 dBFS, rising spectrum, big DC |
| 120 kHz | 7.68 MHz  | dead: all zeros |
| 128 kHz | 8.19 MHz  | broken (as 112 kHz) |
| 192 kHz | 12.3 MHz  | refused by driver: "sample rate is too large" |

Spectrum at 104 kHz (left, dBFS per half-octave band, 2 captures):
4k -76..-79, 8k -79..-81, 11k -80, 16k -72..-73, 22.6k -73..-75,
32k -78, 45k -80. No roll-off visible to 45 kHz; floor is flat at about -78 to
-80, with a bump at 16-23 kHz. No ultrasonic source was available, so this is
noise floor, not a measured response.

## Raw PDM (no decimation) test, 2026-10-15 (current pair)

`audio pdmraw 8 3 rate <hz>` then `audio pdmbits 1` (new command). Bit clock =
32 x rate. Control: floating GPIO4 as DAT reads 100.00% ones, 0.00% flips.

| bit clock | ones (slot 0/1) | flips | window-stdev | result |
|-----------|-----------------|-------|--------------|--------|
| 3.07 MHz  | 49.98 / 49.98 % | 70.1 % | 0.072 % | alive |
| 4.10 MHz  | 49.98 / 49.96 % | 70.1 % | 0.073 % | alive |
| 4.61 MHz  | 49.97 / 49.96 % | 70.1 % | 0.072 % | alive |
| 4.86 MHz  | 49.98 / 49.96 % | 70.1 % | 0.074 % | alive |
| 4.99 MHz  | 49.98 / 49.96 % | 70.1 % | 0.075 % | alive (last that opens) |
| >= 5.12 MHz | -- | -- | -- | driver: "sample rate is too large" |

Raw mode is capped lower (~5 MHz) than filtered mode (6.66 MHz): the driver
needs mclk = 16 x bit clock < 80 MHz in raw mode. Mic response to sound not
yet exercised in raw mode (no stimulus applied during the run).

## Ultrasonic mode (`audio pdmultra 8 3`), 2026-10-15, current pair (set G)

**SUPERSEDED:** these numbers were taken while the firmware overran the
receiver (160 MHz, -Og); the -50 dBFS RMS and the rising floor are artefacts.
See `pdm_mic_findings.md` for the corrected results.

Start at 2.048 MHz, 100 ms, then 4.8 MHz in place; software sinc^4 decimation
by 16 -> 300 kHz PCM; spectrum limited to maxhz = 80 kHz. Bit order in each raw
word is MSB-first (LSB-first gave a -27 dBFS rising mess).

| run | L rms | R rms | L stdev | R stdev |
|-----|-------|-------|---------|---------|
| 1   | -50.6 | -50.4 | 97.0    | 99.0    |
| 2   | -50.5 | -50.3 | 98.2    | 100.1   |
| 3   | -50.6 | -50.6 | 96.6    | 97.1    |
| 4   | -50.4 | -50.3 | 98.4    | 99.7    |

L-R about 0.1 dB. Broadband RMS is ~7 dB above the 48 kHz figure because it now
includes 20-150 kHz noise. Left-channel band power (dBFS), 4 runs:
4k -63..-66, 16k -57..-62, 32k -55..-60, 64k -53..-58 -- rising about
1.3 dB per half octave, which is what a white noise floor does; no roll-off to
80 kHz. Sinc droop (~4 dB at 80 kHz) is not compensated.

## Ultrasonic response vs datasheet, 2026-10-15

Plot: `results/pdm_ultrasonic_response.png` (datasheet curve read off the
graph on sheet 7, +-0.5 dB; measured noise PSD from 6 x 32767-frame raw dumps
at 4.8 MHz, decimated offline by sinc^6 x16, Welch/Hann).

Measured noise floor is flat at about -118 dBFS/Hz from 4 to 20 kHz (-120 to
-115 to 60 kHz), with narrow spurs at 15.7, 22.4, 28, 31.3, 54.5 and 62.5 kHz
(62.5 kHz about 25 dB above the floor). Both slots agree. This is an electrical
noise floor with no acoustic stimulus; it cannot be compared with the
datasheet's acoustic response (normalized to 1 kHz, +14.6 dB peak at 25 kHz,
about +8 dB from 40 to 65 kHz, rising to +12.8 dB at 80 kHz). That needs a
known ultrasonic source.

Firmware findings made on the way: the first on-device results (floor -65 dBFS
per band at 4 kHz) were receiver overruns. Needed: decimator and capture.c at
-O2, and 240 MHz CPU (project default is 160). With both, on-device bands match
the offline analysis (-84 to -89 dBFS at 1-16 kHz). Decimator order is now 6.

## Spur vs mic clock test, 2026-10-15

Raw dumps at 4.8, 4.0 and 3.2 MHz (all in ultrasonic mode), 4 x 32768 frames
each, sinc^6 x16 offline, slot 0. Spurs (>= 8 dB over floor) did not move:
15.6, 22.5, 28.05, 31.3, 54.5, 62.65 kHz at all three clocks (62.65 kHz is
+22 to +24 dB each time). Floor -115.5 / -116.8 / -115.9 dBFS/Hz. The spurs
are therefore not products of the PDM clock or modulator; they come from
outside it (acoustic, or electrical pickup).

## Outdoor vs indoor, ultrasonic mode, 2026-10-15

Plot: `results/pdm_outdoor_vs_indoor.png`. 6 x 32768 raw frames each, 4.8 MHz,
sinc^6 x16 offline. Integrated band power, dBFS (slot 0 / slot 1):

| band       | indoor        | outdoor       |
|------------|---------------|---------------|
| 0.1-1 kHz  | -66.8 / -66.4 | -68.7 / -68.2 |
| 1-8 kHz    | -72.6 / -71.5 | -72.0 / -71.4 |
| 8-20 kHz   | -76.5 / -73.8 | -72.6 / -70.8 |
| 20-80 kHz  | -66.7 / -66.4 | -68.8 / -67.8 |

Median floor 5-80 kHz: -118.0/-116.1 indoor, -116.6/-115.3 outdoor dBFS/Hz.
All narrow spurs (15.6, 22.5, 31.3, 54.5, 62.6 kHz) are gone outdoors; only a
5-6 dB wobble at 19-20 kHz remains. L-R coherence outdoors: 0.64 (0.1-1 kHz),
0.46 (1-8 kHz), 0.2 above 8 kHz.
