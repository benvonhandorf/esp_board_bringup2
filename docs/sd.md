# SD

[← Command reference](README.md) · [Project README](../README.md)

Brings a card up over any of the three interfaces a board might have wired, and
measures how fast it actually goes. Which interface a board provides, and
whether the card keeps up, is exactly the kind of thing that has to be
established before a BSP exists.

Bring-up is deliberately in two stages. `sd spi` and `sd mmc` initialize the
card and nothing more; the filesystem is only mounted when `sd bench` needs one.
That way a blank, corrupt or non-FAT card still reports its identity through
`sd info` and is still measurable through `sd raw`, instead of the whole thing
unwinding because there was no FAT partition to mount.

**Neither benchmark writes to the card outside a filesystem.** `sd bench` creates
`/sd/bench.tmp`, reads it back and deletes it; `sd raw` is read-only. Nothing
that was on the card is disturbed.

## `spi <clk> <mosi> <miso> <cs> [khz <freq>]`

Brings the card up over SPI. This is the only option on chips without an SD host
peripheral — notably the ESP32-C3, where `sd mmc` is not compiled in at all.

The frequency is a `khz` keyword pair rather than a trailing number because
`mmc` below takes a variable number of pins, and a bare number could not be told
apart from another pin. It defaults to 20 MHz; 40 MHz is the high-speed rate.
What the host divider actually produced is reported, since it quantizes.

Re-running the command tears the previous configuration down first, so pins and
frequency can be changed freely.

## `mmc <clk> <cmd> <d0> [<d1> <d2> <d3>] [khz <freq>]`

Brings the card up on the dedicated SD host. Three pins select 1-bit mode, six
select 4-bit — the pin count is what picks the width, so there is no separate
and forgettable width argument.

The width is applied to the *slot*, not to the host flags. ESP-IDF reads the
slot width back and narrows the host to match, so setting the slot is what
actually stops a 1-bit slot from being switched to 4-bit partway through
initialization.

Internal pull-ups are enabled on the bus. They are weak and no substitute for
proper external ones, but a board being brought up frequently has none fitted at
all, and getting the card to answer is the point of the exercise.

Note that a card put into SPI mode by `sd spi` **stays** in SPI mode until its
power is removed; that is the card's behaviour, not this tool's. A board reset
does not do it. Test SD mode first, or physically power-cycle in between.

## `info`

Reports the detected card: the interface and pins in use, card type, product
name, the CID (manufacturer, OEM, revision, serial and manufacture date),
capacity and sector geometry from the CSD, negotiated bus width, and the
filesystem's total and free space when one is mounted.

Two clocks are reported, not one. The first is what the host is really clocking
the card at, the second is the ceiling the card itself advertises — together
they say whether the interface or the card is the limit.

## `bench [size_kb] [block_kb]`

Mounts FAT if it is not already mounted, then writes, reads back and deletes
`/sd/bench.tmp`. Defaults to 512 KB in 16 KB blocks. The result is appended to
the results file described below.

The write measurement includes the final flush. Without it the card is still
absorbing the tail of the data when the clock stops, and the figure reported is
the speed of filling a RAM buffer rather than the speed of the card.

Every block is verified against what was written, and each carries its block
index, so a filesystem handing back the wrong block is reported rather than
scored. The comparison is done outside the timed region and is not charged to
the card.

## `raw [size_kb] [block_kb] [start_sector]`

Reads whole sectors straight off the card with no filesystem in the way. The
gap between this and `bench` is what FAT costs.

The read is read-only, so it is safe to run anywhere on the card. Blocks are
rounded down to whole sectors and the range is checked against the card's
capacity. The result is appended to the results file described below, which is
the one write it does make.

## `sweep [max_khz] [size_kb] [block_kb]`

Steps the clock up and reports the fastest rate the card still returns *correct*
data at. Defaults to a ceiling of 80 MHz and 512 KiB read per step.

This exists because an overclocked card usually does not fail — it succeeds and
hands back corrupt data. A sweep that only checked for errors would report a
confident, entirely wrong answer. So the sweep first reads the test region at
20 MHz, a rate every card is rated for, and keeps a CRC32 of it; every faster
step re-reads the same sectors and compares. A CRC rather than a kept copy
because it costs four bytes instead of a second buffer, which is what lets the
verified region be big enough to mean something.

The reference is read **twice** and the two must agree. A card that cannot
reproduce its own data in spec makes every later comparison meaningless, and
that is worth saying outright rather than deriving an overclocking limit from
noise.

The measurement is **read-only**. Nothing is written to the card at a clock that
has not been verified, because a corrupt write is not recoverable the way a
corrupt read is. The results file is written afterwards, once the card has been
reopened at the fastest rate that passed.

Two things the report is careful to distinguish:

- **The card failed** — a data mismatch, a read error, or an init failure. The
  first such rate is reported.
- **The host ran out of clock.** The dividers quantize, so several requested
  rates land on the same actual one; that is normal and those steps are skipped.
  But when *every* larger request produces an identical clock, the card was
  never driven any faster and its real limit is still unknown. That is reported
  as a host limitation, not as a pass.

Steps that pass above the card's CSD-rated speed are marked `(overclocked)`.
Passing one read sweep is not a stability guarantee — it is out of spec, and the
margin varies with temperature, supply and wiring.

The rating is re-read at every step rather than measured once, because it is not
constant. ESP-IDF only attempts the CMD6 high-speed switch when the host asks
for more than 20 MHz; below that the card stays in Default Speed and reports
25 MHz, and after the switch it reports 50 MHz. Comparing every step against the
figure seen at the 20 MHz reference would label perfectly in-spec High Speed
operation as overclocking.

Afterwards the card is left initialized at the fastest verified rate, so
`sd bench` and `sd raw` measure it without re-entering anything.

## `results [clear]`

Prints the saved results file back, or with `clear` deletes it. Worth having
because on a bringup bench the board is usually the only thing holding the card,
so there is no convenient way to pull it and read the file on a host.

## `close`

Unmounts, releases the card and frees the bus.

## The results file

`bench`, `raw` and `sweep` each append their output to **`/sd/sdbench.txt`** when
they succeed. Nothing is overwritten, so a card accumulates a history and can be
carried between boards.

Each entry is stamped with enough identity to say where it came from, which is
the point — a bare table of numbers found on a card months later is worthless:

```
================================================================
SD card clock sweep / overclocking test
Uptime:    25 s when run. The board has no RTC, so entries are in
           file order, not wall-clock order.
Chip:      ESP32-S3 rev v0.2, 2 cores
Flash:     8192 KB
MAC (STA): d8:3b:da:45:4c:ec  <- identifies this board
Firmware:  esp_board_bringup 3500f4b-dirty (built Aug 12 2026 10:44:18)
ESP-IDF:   v6.0.1
Interface: SPI on CLK=GPIO7, MOSI=GPIO9, MISO=GPIO8, CS=GPIO21
Clock:     20.000 MHz (requested 20000 kHz), card rated 25.000 MHz
Card:      SD04G, SDHC/SDXC, 3.68 GiB, CID mfg 0x27 serial 0x7C559EEF, made 2015-05
----------------------------------------------------------------
```

The station MAC is the part that is genuinely unique per board; the chip model
and revision say which design, and the pins are named by role so the wiring
harness is recorded too. For a sweep, the whole rate-by-rate table follows,
including which steps were overclocked and where it stopped and why.

The body is captured with the same formatting calls that produced the console
output, so what is saved cannot drift from what was displayed.

Saving is **best effort**. The measurement has already succeeded by the time the
file is written, so a card with no filesystem produces a plain note rather than
an `ERR:` line — a host script parsing `ERR:` should not be told a good
benchmark failed because there was nowhere to record it.

## Reported numbers

Both benchmarks report the average throughput **and the slowest single block**.
The worst case is not a footnote: SD cards stall for tens of milliseconds while
they do internal housekeeping, and on a board that has to keep up with a sensor
or a camera that stall is what decides whether the design works. An average
hides it entirely.

Sanity-check the result against the interface ceiling — at the 20 MHz default
that is 2.5 MB/s on SPI or 1-bit SD, and 10 MB/s on 4-bit SD, scaling with the
clock. A number above the ceiling means something was measured other than the
card.

Measured on the board this was developed against, a XIAO ESP32-S3 Sense with a
4 GB SDHC card (`SD04G`, rated 25 MHz), 512 KiB in 16 KiB blocks:

| | SPI @ 20 MHz | SD 1-bit @ 20 MHz | SD 1-bit @ 40 MHz |
|---|---|---|---|
| Raw read | 1.45 MiB/s | 2.20 MiB/s | 4.19 MiB/s |
| FAT read | 1.43 MiB/s | 2.19 MiB/s | 4.08 MiB/s |
| FAT write | 0.39 MiB/s | 0.39 MiB/s | 1.43 MiB/s |
| Worst write block | 27.7 ms | 849.5 ms | 33.7 ms |

And on an M5Stack Cardputer (SPI only — the slot breaks out just CLK 40, MOSI
14, MISO 39, CS 12) with an 8 GB SanDisk `SA08G` from 2015:

| | SPI @ 20 MHz |
|---|---|
| Raw read | 1.42 MiB/s |
| FAT read | 0.93 MiB/s |
| FAT write | 0.21 MiB/s |
| Worst write block | 885.7 ms |

The raw read matching the XIAO's 1.45 MiB/s across two different cards is the
useful part: at 20 MHz over SPI the bus is the limit, not the card. Where the
cards differ is writes — 0.21 MiB/s with an 885 ms worst-case block is this
particular card's flash management, and it is why `bench` reports the worst
block alongside the average.

`sd sweep` found the limits to be 20 MHz over SPI and 40 MHz over SD 1-bit. Both
are worth understanding, because neither is the card — and neither is an
overclock, despite 40 MHz being above the 25 MHz the card advertises at default
speed:

- **Over SPI it stops at 20 MHz, and this is a protocol threshold rather than a
  speed limit.** Initialization fails in `sdmmc_enable_hs_mode_and_check()`
  re-reading the CSD. That looks like a signal-integrity ceiling and is not one:

  `sdmmc_init_card_hs_mode` runs *before* `sdmmc_init_host_frequency`, so the
  failing SEND_CSD is issued at the 400 kHz initialization clock — the bus never
  reaches the requested rate. The switch is attempted only when
  `host.max_freq_khz > SDMMC_FREQ_DEFAULT`, which is 20000 exactly, so what
  triggers it is crossing a constant in software.

  Confirmed directly: **20000 kHz initializes and 20001 kHz does not**, with the
  same `send_csd returned 0x108`. A 1 kHz difference cannot matter electrically,
  and the SPI divider quantises both to the same clock anyway.

  What the card does is accept the CMD6 high-speed switch and then fail the
  SEND_CSD that follows it. `sdmmc_init_card_hs_mode` tolerates
  `ESP_ERR_NOT_SUPPORTED` by falling back to 20 MHz, but treats every other
  error as fatal, so this aborts the whole init instead of degrading. Two
  different cards on two different boards behave identically, so it is not
  specific to a card, a board, or the GPIO matrix.
- **Over SD 1-bit it stops at 40 MHz** — that is the host, not the card, and it
  is a fixed divider rather than a negotiated rate. ESP-IDF's
  `sd_host_slot_get_clk_dividers()` maps *every* request of 40 MHz or more onto
  `host_div = 4`, i.e. 160 MHz / 4 = exactly 40 MHz, so the card was never
  driven faster and its own limit is unknown. See the note below.

  Note that 40 MHz is **not** an overclock for this card: once it accepts the
  high-speed switch its CSD reports 50 MHz, so the whole sweep ran in spec.

### Why 40 MHz is the SD-mode ceiling

The ESP32-S3's SD host is fed only by PLL_160M or the 40 MHz crystal — there is
no 200 MHz source, and `SOC_SDMMC_UHS_I_SUPPORTED` is not defined for it, so
UHS-I (SDR50 at 100 MHz, SDR104 at 208 MHz) does not exist on this chip. That
puts SD High Speed, specified to a 50 MHz maximum, at the top of what the part
can do.

Dividing 160 MHz by an integer, the options either side of that limit are
160/4 = 40 MHz and 160/3 = 53.3 MHz. The latter is over the 50 MHz High Speed
ceiling, so 40 MHz is the fastest in-spec rate the clock tree can produce, and
ESP-IDF hard-codes it for any request at or above 40 MHz.

So the 40 MHz wall is not a silicon divider limit — the hardware could emit
53.3 MHz or 80 MHz — but there is no way to ask for those through the public
API, and both would be outside the SD specification.

Note the 849.5 ms worst-case write block at 20 MHz, against 33.7 ms for the same
card at 40 MHz. That is not the clock — it is the card pausing for internal
housekeeping, landing in one run and not the other, and it is large enough to
drag the whole 20 MHz write average below the SPI one. It is precisely why the
worst block is reported.

## Example: Seeed XIAO ESP32-S3 Sense

The Sense expansion board's microSD is on CLK/SCK `GPIO7`, CMD/MOSI `GPIO9`,
D0/MISO `GPIO8` and CS `GPIO21`:

```
sd spi 7 9 8 21     # SD over SPI
sd close
sd mmc 7 9 8        # SD 1-bit on the same pins
```

D1, D2 and D3 are not brought out on that board, so 4-bit mode cannot be
exercised there even though the command supports it.
