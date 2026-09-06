# strings/

Every sentence this firmware shows a user, authored as JSON and compiled onto the `res`
partition rather than into the image. One subdirectory per locale, one file per command
group — the same split as `docs/<group>.md`.

Only `en-US` exists today. It is the base locale: ids come from it, and it is what the
compiler checks a translation against.

## Why not just write the string in the C file

A literal in the firmware occupies the OTA slot, which is the tight resource here — the
image is 1.50 MB against 1.69 MB — and it is duplicated across both slots. The same
string here costs one copy of a partition that OTA does not touch. Moving all thirty
command groups took 58 kB out of the slot; the blobs that replaced them are 65 kB on a
`res` partition that is still 446 kB empty.

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
  plus a lookup costs more than they do. The same goes for one- and two-word labels
  printed inside a wider sentence: reset reasons, `"pull-up"`, `"driven low"`.
- Anything printed before `strres_init()` succeeds, including whatever reports that the
  strings could not be loaded. That covers the `ESP_LOG` lines in `main.c` and
  `config_reader.c`, which run before the filesystem is mounted.
- HTTP status text in `app_http.c`. It is protocol, and it is read by a browser rather
  than by someone at the console.
- Command names, group names, config keys, JSON templates, and the command lines
  `board.c` feeds to `cli_execute()`. Those are identifiers, not prose.

## Consequences worth knowing

The compiled blobs land in `fs/str/`, which is gitignored — this directory is the
source. A build whose strings changed must be **flashed**, not OTA'd, because OTA does
not write `res`; otherwise the affected lines come out as `[str:XXXX]`. That is the
catalogue check working, and `tools/`-side decoding is in the `strres_ids.txt` the build
emits under `build/strres_gen/main/`.
