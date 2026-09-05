# tools/

Host-side scripts. None of this is firmware.

| | |
|---|---|
| `flash_a.py` | Flash, and erase `otadata` so the slot just written is the one that boots. `idf.py flash` alone leaves a device that has taken an OTA running the *old* firmware, silently. |
| `ota_upload.py` | Push a new image over HTTP. The header is checked after about a kilobyte, so a wrong binary is refused quickly and by name. |
| `get_status.py` | Fetch and print `/api/status`. |
| `mqtt_tail.py` | Follow the device's MQTT topics, including the forwarded log. |
| `bringup.py` | Drive the console over the serial port. Importable as a module — it exports `Console` and `Checks` for test scripts. |
| `check_docs.py` | Verify that `main/app_console.c` and `docs/` agree in both directions. |

## bringup.py

```sh
./bringup.py "sys info" "gpio survey"
BRINGUP_PORT=/dev/ttyACM1 ./bringup.py "i2c scan"
```

The one piece worth reading is `read_until_prompt()`. The obvious way to know a
command has finished is to wait for the port to go quiet, and it does not work
here: `audio tone 1000 3` prints nothing at all for three seconds while it plays,
and a quiet-timeout gives up in the middle of it. Waiting for the prompt instead
is what makes the long-running commands drivable.

## check_docs.py

```sh
./check_docs.py
```

Reads the command tables out of `main/app_console.c` and checks that every
registered command has a heading in its group's docs page, and that no docs page
names a command that does not exist.

This exists because the two drift silently, and in the direction that matters
most: a manual or an error message naming a command that has been renamed is read
by someone who is already stuck. It caught eighteen such references during the
port from nested menus to flat groups.

Adding a group means adding a row to `PAGE` naming the docs page that covers it,
or the check fails — which is the point.
