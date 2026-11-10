# Handoff: `audio-net` at 300 kHz stereo delivers about 79% of the audio

Status: **open, abandoned for now (2026-11-10).** Nothing below is a conclusion except where it says measured.

## The problem

Streaming the `pdmultra` source at its 300 kHz decimated rate, two channels (9.6 Mbit/s nominal),
over `GET /api/audio/stream`:

| Run | Received | Device-side counters |
|---|---|---|
| 1 | 946.8 kB/s (78.9%) | 0 dropped, 0 read errors, buffer high-water 319 ms |
| 2 | 946.4 kB/s (78.9%) | 0 dropped, 0 read errors, buffer high-water 376 ms |
| 3 | 909.1 kB/s (75.8%) | 0 dropped, 0 read errors, buffer high-water 945 ms |

(1200 kB/s is nominal.) A WiFi limit would fill the buffer and show as `Dropped ...` in
`audio-net status`; it did in other runs (a ~1.5 s stall, 309 kB dropped) but not in these. So the
samples appear never to have been produced. Not yet proven: nothing counted frames produced.

Reproduce (Sinn board, four SPH0641 mics on CLK=10, DATA=11,12):

```sh
./tools/bringup.py "audio close" "audio pdmultra 10 11 maxhz 100000"   # streams at 300 kHz
python3 <scratchpad>/pull.py <device ip> 15                            # see below
./tools/bringup.py "audio-net status"
```

`pull.py` was a throwaway in the session scratchpad: it GETs the stream for N seconds, parses the WAV
header, prints bytes/s against nominal and per-channel level. Recreate it if needed (about 60 lines).

## What is known (measured)

- **Same source, one channel** (`?ch=0`): 97-100% of nominal at 300 kHz (600 kB/s), 0 dropped. So
  decimation at 300 kHz can keep up in the stream task when the output is mono.
- **Hardware filter, four channels** (`audio pdm 10 11,12 rate 64000|75000 ultra`): 100.3% at 64 and
  75 kHz, 0 dropped, **after** raising the TCP send window (below). 16 and 32 kHz clean too.
- **Software raw path** (`pdmultra ... raw`) at its default 75 kHz: 99.1%, decimator 36.5% of a core.
  Not yet run at 150 or 300 kHz (`audio-net config rate 300000`), which is the natural next test.
- `pdmultra` non-raw decimates **inside `audio_bus_read()`** (`main/audio/i2s_bus.c`, around line 946),
  i.e. in the stream task, at about 240 cycles per frame (30% of one core at 240 MHz).
- The network alone caps near 7.5 Mbit/s here (stereo 300 kHz needs 9.6), so even a fixed capture side
  cannot deliver 300 kHz stereo over this link. The question is the capture shortfall, not WiFi.

## Update: the capture side is confirmed short (later the same day)

`audio-net status` now prints `Produced N frames, X kHz average`. Stereo 300 kHz, two runs, on a link
`wifi iperf` measured at 9.06 Mbit/s (RSSI -48 dBm, `sleeperservice.lan:6002`) with the 32 kB TCP window:

- Produced **233.3 and 233.0 kHz** average against 300 kHz, so about 78%.
- 0 dropped, 0 read errors, buffer peaked at 247-338 ms: the network was not the limit.
- Decimator 271 cycles per frame, 34% of a core.

The same figure reads 75.0 kHz at 75 kHz and 48.1 kHz at 48 kHz, so the counter is honest, and a slow
listener does not disturb capture (stall tests: 48.1 kHz produced through a 2.5 s client pause).
Back-of-envelope: 233 kHz is about 1030 cycles per frame at 240 MHz, of which the decimator is 271, so
roughly 760 cycles per frame in the rest of the stream task or waiting on something.

## What is not known

- Why stereo, not mono, is affected. Both do the same decimation. Stereo does twice the convert, ring
  write (PSRAM) and drain/send work. Guesses, none tested: the drain task shares core 1 with the stream
  task (`qc.core = -1` in `audio_stream_http_serve()`); PSRAM write cost; the receiver overrunning
  silently (the DMA loses samples without a read error, as AGENTS.md warns for ultrasonic capture).
- **Whether USB shows it.** Untested. The capture, decimation and conversion are shared with USB; only
  the final write differs. But USB cannot carry the failing case: the S3 FIFO allows 300 kHz on one
  channel only, or two channels up to 150 kHz, so a USB run can reproduce only the mono case, which
  works. Not a useful discriminator for this symptom.

## Suggested next steps (cheapest first)

1. Pin the drain task to the other core from the stream task (`qc.core` in `audio_stream_http_serve()`,
   currently -1) and compare `Produced`.
2. Run the raw path at 300 kHz (`audio pdmultra ... raw`, then `audio-net config rate 300000`), which
   moves decimation out of `audio_bus_read()` and into the runner's own decimator.
3. Time the stages of the stream task (read, decimate, convert, ring write) with the cycle counter.
4. If capture really loses samples, look for the I2S RX queue overflow event in `i2s_bus.c`: a read
   timeout is the only overrun signal today, and none occurred.

## State of the work

- **Board** (Sinn, `/dev/ttyACM0`) runs the real `build_sinn` image (TCP window 32 kB). TX power is a run-time
  setting and was reset by the USB test's reboot: **the board will not join WiFi at the default 20 dBm**;
  `wifi txpower 8`, `wifi off`, `wifi on` (see `docs/wifi.md`).
- **Uncommitted** in this repo: the `audio-net` and `audio-usb` port, strings, docs, `sdkconfig.defaults`
  (TCP window), AGENTS.md, this file. `dependencies.lock` is restored to HEAD on purpose.
- **`esp_components`**: one local commit (`ac87fd6`) with annotated tags `audio_stream-v0.1.0`,
  `audio_stream_http-v0.1.0`, `audio_stream_usb-v0.1.0`. **Not pushed.** Until they are, a plain
  `idf.py build` here fails; AGENTS.md ("A new component needs a commit and a tag") has the local recipe.
- `sdkconfig.sinn` was regenerated to pick up the new defaults (old copy kept in the session scratchpad
  only). The other generated `sdkconfig*` files still hold the old 5760 B window until they are regenerated.
