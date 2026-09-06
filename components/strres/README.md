# strres

User-visible strings kept on a filesystem instead of in the application image.

A string compiled into the firmware occupies the OTA slot — the scarce resource on a
4 MB part — and is duplicated across both slots. The same string on a data partition
costs one copy of a partition OTA does not touch. For a device with a command shell,
that is tens of kilobytes of slot headroom, and it makes a second language a file
rather than a rebuild.

Strings are authored as JSON, compiled to indexed binary blobs at build time, and
named from C by generated ids — so the call site stays readable and the words are
not in the binary.

## Installing

```yaml
dependencies:
  strres:
    git: https://github.com/benvonhandorf/esp_components.git
    path: strres
    version: strres-v0.1.0
```

## Using it

Author one JSON file per section. A section is usually one command group.

```json
{
  "section": "i2c",
  "strings": {
    "bus.help":   "Initialize the I2C bus on the given pins",
    "responding": "%d device%s responding\n",
    "no_bus":     { "text": "No I2C bus. Run 'i2c bus <scl> <sda>' first.",
                    "pin": true }
  }
}
```

Compile them from the project's `CMakeLists.txt`. `IMAGE_DIR` must be inside the
directory the filesystem image is packed from:

```cmake
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    strres_generate("${CMAKE_CURRENT_LIST_DIR}/../strings"
                    IMAGE_DIR "${CMAKE_CURRENT_LIST_DIR}/../fs/str"
                    OUT_C STRRES_C OUT_INCLUDE_DIR STRRES_DIR)
endif()
```

Add `${STRRES_C}` to `SRCS` and `${STRRES_DIR}` to `INCLUDE_DIRS`. Then, once the
filesystem is mounted:

```c
#include "strres.h"
#include "strres_ids.h"

strres_init(NULL, &strres_generated_catalog);

STRRES_PRINTF(STR_I2C_BUS_HELP);
STRRES_PRINTF(STR_I2C_RESPONDING, count, count == 1 ? "" : "s");
STRRES_ERROR(STR_I2C_NO_BUS);
```

## The arguments are still checked at compile time

Moving a format string out of the binary would normally cost `-Wformat`, since the
compiler can no longer see it. `STRRES_PRINTF` gets it back:

```c
STRRES_PRINTF(STR_I2C_RESPONDING, "three");
error: format '%d' expects argument of type 'int', but argument 2 has type 'char *'
```

It works by naming the English text inside an `if (0)` branch. GCC folds that away
before optimisation, so the text reaches neither the object file nor the link at any
`-O` level — the diagnostic is all that survives. Translations are checked separately:
the compiler rejects any locale whose conversion specifiers differ from the base.

## Buffer ownership

**strres never returns a pointer the caller must free.** There is one pairing in the
whole API and it is explicit.

| | Who owns what |
|---|---|
| `strres_printf` / `strres_error` | Nothing. The text does not cross the boundary. |
| `strres_copy` | The caller's buffer, which it already owns. |
| `strres_hold` / `strres_release` | A borrowed pointer, valid until the release that must follow it. |

Prefer the first. It covers almost every use, and it is the only one with no lifetime
question at all.

## Memory

A section is read whole into one allocation and cached, because the access pattern is
bursty: printing one group's help touches a dozen strings that all live in the same
file, which is then one filesystem read rather than a dozen.
`CONFIG_STRRES_CACHE_SECTIONS` slots are kept (default 3, a few kilobytes each), with
least-recently-used eviction that never takes a section something holds. The pinned
section — every string marked `"pin": true` — is loaded once at init and never
evicted.

Nothing grows past the configured budget: a lookup needing a slot while every slot is
held misses and prints its id, rather than allocating.

## Things this does that will not look like errors

- **A missing string prints `[str:04F2]`, not a name.** A table of names would put
  strings back into the image, which is the thing being avoided. Decode the id with
  the `strres_ids.txt` the build emits, or set `CONFIG_STRRES_DEBUG_NAMES`.
- **A catalogue mismatch makes every lookup miss, deliberately.** The firmware
  carries a hash of the id assignment and every blob repeats it. After an OTA that
  changed the strings, the device is running new firmware against the resources the
  last *flash* wrote — printing `[str:...]` says so plainly, where printing a
  sentence that means something else would not. Reflash the partition.
- **The hash covers the id assignment, not the text.** Fixing a typo does not strand
  a device whose partition is a build behind; it shows the old wording, which is
  stale but true. Adding or removing a string does shift ids, and that is exactly
  when the resources must be reflashed.
- **Strings needed before `strres_init()` succeeds must stay C literals** — including
  whatever reports that the strings could not be loaded.

## Testing

```sh
make -C test
```

Host test, plain gcc. It compiles the fixtures with the real compiler first, so a
change to the on-disk format or to the id assignment fails there rather than on
hardware.

## License

Same as the rest of `esp_components`.
