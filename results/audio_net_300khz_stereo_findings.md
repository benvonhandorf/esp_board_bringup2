# Findings: `audio-net` 300 kHz stereo capture (2026-11-11)

Follows `audio_net_300khz_stereo_shortfall.md`. Sinn board, four SPH0641 mics, `build_sinn` at 11bb57f plus
the changes listed under "What is kept". Source: `audio pdmultra 10 11 maxhz 100000` (300 kHz, stereo, software
decimation), pulled with `GET /api/audio/stream` for 15 s.

## Result

| State | Received | Produced | Dropped |
|---|---|---|---|
| Start of session (TX 8 dBm) | link unusable, 0.9 Mbit/s iperf | n/a | n/a |
| TX power 4 dBm, no code change | 78-80% | 241 kHz | 0 |
| + convert fast path | 81-84% | 292-293 kHz | 1.2-1.8 MB (sender) |
| + 16 kB send chunks (**kept**) | 97.8-98.3% | 293.5-294.7 kHz | 0 |

Produced is 98% of 300 kHz. Not 100%; see "What is still short".

## Findings (measured)

1. **Bandwidth was the TX power ceiling, and lower is better.** iperf2 to `sleeperservice.lan:6002`:
   8 dBm 0.9 Mbit/s, 12 dBm 0.45, 6 dBm 14-15, 5 dBm 16-17, 4 dBm 18-22. At 14 dBm and above the board does not
   rejoin (the known RF-stage fault, `docs/wifi.md`). 300 kHz stereo needs 9.6 Mbit/s, so use `wifi txpower 4`.
   The handoff's "8" is wrong for this link: RSSI was -31 dBm throughout, so it is not range.
2. **Capture was CPU-bound in the stream task.** The budget is 800 cycles per frame (240 MHz / 300 kHz). Before
   the fix the loop took about 990: decimator 255, `audio_stream_convert()` 280, sink write 122, rest ~330.
   Above 800 the I2S RX queue discards the oldest buffer silently, so there are no read errors and no drops.
   The produced rate tracks 800/loop-cycles closely (800/820 = 97.6% against 97.4% observed).
3. **Convert was the cheapest win.** The 16-bit to 16-bit case went through a per-sample width dispatch; as one
   `memcpy` it is 6 cycles per frame.
4. **A second limit was the sender, not capture.** With the 4 kB default chunk each `httpd_resp_send_chunk()` is
   three socket sends; received throughput stuck near 7.8 Mbit/s with the ring full and 1.2-1.8 MB dropped per
   run, though iperf showed 18+ Mbit/s. 16 kB chunks removed it.
5. **The PSRAM ring write costs more than its own cycles.** A sink that discards the data: produced 299.2-299.5
   kHz, and the decimator, which never touches PSRAM, fell from 255 to 158 cycles per frame. The sink's own
   write is only ~55 cycles, so the rest is contention on the bus flash and PSRAM share (stream task writes,
   drain task reads, decimator code fetches from flash). Likely, not proven.
6. **Hardware limit of the four-channel path.** `audio pdm ... ultra` refuses rates above 75 kHz
   (`needs a rate from 48000 to 75000 Hz`), and the software path reads one data line only. So four mics at
   150 ksps is not reachable today. Four at 75 ksps (measured 100.3%) and two at up to 300 kHz are.

## What is kept

- `audio_stream_convert()` fast path for 16-bit to 16-bit (in `../esp_components/audio_stream/src/audio_stream_fmt.c`,
  **uncommitted, untagged, unpushed**). One copy when every source channel is selected in order, otherwise a
  plain 16-bit loop; other widths take the old path. `test_convert` gained identity and one-channel cases;
  `make -C ../esp_components/audio_stream/test` passes (76 checks).
- `SEND_CHUNK_BYTES` = 16384 in `main/audio/net_stream.c`, passed as `queue.chunk_bytes`. The component already
  honours the option, so nothing in `esp_components` changed for it. Costs 16 kB of internal RAM.
- Needs: commit and tag `audio_stream` in `esp_components`, push, then bump the pin in `main/idf_component.yml`
  and refresh `dependencies.lock` (AGENTS.md, "A shared component is pinned to a git tag"). Until then a plain
  build does not contain the fast path, because `managed_components/` is a fetched copy.

## Tried and dropped

- **Decimator in IRAM** (`linker.lf`, `noflash_text`): produced +2.7 kHz (292.3 to 295.0), decimator 255 to 182
  cycles. Cost: internal free RAM fell from 174 KB to 119 KB, low water 4 KB. Not worth it on its own.
- **Stream task, convert and queue in IRAM as well**: no further gain (295.7 kHz) and internal RAM hit 0 KB low
  water, which collapsed the sender to 16-21% received. Do not.
- **Ring in internal RAM**: it can only hold ~49 kB (39 ms), against 366 ms peak in PSRAM, so it overflowed
  and dropped 80% of the stream. Not a valid comparison and not viable.
- Pinning the drain task (handoff step 1) was not needed: findings 2 and 5 say the cost is in the stream loop and bus.

## What is still short

About 2% (294 against 300 kHz). Still a stream loop of about 815 cycles per frame at the 800 budget.
Internal free RAM during a stream is thin: 121 KB, low water 2 KB (`sys info`). Treat that as a limit on
any further buffer or IRAM growth.

## Possible approaches

Cheapest first; none tried.

1. **Count the overflow.** Hook `I2S_EVENT_RX_Q_OVF` in `i2s_bus.c` and report it in `audio-net status`, so the
   loss is measured and not inferred from the produced rate.
2. **Write the ring in bigger, rarer blocks.** Findings 5 points at the number and size of PSRAM transactions.
   Assemble a few stream blocks in internal RAM, then one bulk copy to the ring. Trades a little latency and
   RAM for less contention.
3. **Move the ring write off the stream task.** Hand finished blocks to a task on the other core (core 0 also
   runs WiFi, so check) through a small internal queue, leaving the capture loop to read, decimate, convert.
4. **Time the I2S read itself.** About 330 cycles per frame were unaccounted for before the fix. Bracket
   `i2s_channel_read()` and separate copy from wait.
5. **Larger DMA ring.** `DMA_DESC_NUM` 6 and `DMA_FRAME_NUM` 240 hold 4.8 ms. Doubling it costs ~6 kB of DMA
   memory and rides out bursts of preemption; it cannot fix a sustained average above 800 cycles.
6. **Reduce decimator work.** 255 cycles per frame is the biggest stage left. A shorter filter or the
   vector (SIMD) stage 2 used by the two-stage filter (`audio pdmbench` compares them) may be cheaper per frame.
7. **Octal PSRAM or the S3 at 120 MHz PSRAM.** Not available on this board (quad, 80 MHz). A different board
   could remove the bus contention outright.
8. **Four channels above 75 ksps.** Needs two-line raw capture, which the S3 does not do (AGENTS.md "Raw PDM
   captures one line only"), or a second receiver. Software decimation for four channels would roughly
   double the decimator's share, to about 45% of a core by estimate.

## Context

The microphones are good to about 80 kHz, which needs at least 160 ksps. So the 300 kHz test exceeds what the
microphones can use; two microphones at 150-160 ksps is the realistic software target and should sit well
inside the budget (the decimator still runs per raw word, the output and network halve). Not run.

## Housekeeping

- `wifi txpower` is a run-time setting and is lost on reset. Put the 4-5 dBm ceiling in `config.json` or say
  so in `docs/wifi.md` (not done).
- The handoff note's "board will not join at the default 20 dBm" is true; its fallback of 8 dBm is too high.
