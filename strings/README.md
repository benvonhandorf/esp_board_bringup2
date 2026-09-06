# strings/

Every sentence this firmware shows a user, authored as JSON and compiled onto the `res`
partition rather than into the image. One subdirectory per locale, one file per command
group — the same split as `docs/<group>.md`.

Only `en-US` exists today. It is the base locale: ids come from it, and it is what the
compiler checks a translation against.

## Why not just write the string in the C file

A literal in the firmware occupies the OTA slot, which is the tight resource here — the
image is ~1.5 MB against 1.69 MB — and it is duplicated across both slots. The same
string here costs one copy of a partition that is 511 kB empty and that OTA does not
touch. `main/` still holds ~70 kB of literals; each group moved is that much slot
headroom back.

## Adding a string

```json
{
  "section": "i2c",
  "strings": {
    "scan.help":  "Probe the bus and tabulate responding devices",
    "responding": "%d device%s responding\n",
    "no_bus":     { "text": "No I2C bus. Run 'i2c bus <scl> <sda>' first.",
                    "pin": true }
  }
}
```

Print it with the generated id — `STR_<SECTION>_<KEY>`, uppercased with `.` as `_`:

```c
STRRES_PRINTF(STR_I2C_RESPONDING, count, count == 1 ? "" : "s");
STRRES_ERROR(STR_I2C_NO_BUS);
```

The arguments are still checked against the English text at compile time, at no cost in
the image; `components/strres/README.md` explains how.

`"pin": true` puts a string in the section that is loaded once at boot and never
evicted. Worth it for error paths and anything printed often; not worth it otherwise.

## What does not belong here

- Short layout fragments — a column separator, a lone `"\n"`, `" %02X"`. A two-byte id
  plus a lookup costs more than they do.
- Anything printed before `strres_init()` succeeds, including whatever reports that the
  strings could not be loaded.
- Command names, group names, config keys, JSON templates, and the command lines
  `board.c` feeds to `cli_execute()`. Those are identifiers, not prose.

## Consequences worth knowing

The compiled blobs land in `fs/str/`, which is gitignored — this directory is the
source. A build whose strings changed must be **flashed**, not OTA'd, because OTA does
not write `res`; otherwise the affected lines come out as `[str:XXXX]`. That is the
catalogue check working, and `tools/`-side decoding is in the `strres_ids.txt` the build
emits under `build/strres_gen/main/`.
