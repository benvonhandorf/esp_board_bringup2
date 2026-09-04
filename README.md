# ESP-IDF project template

A starting point for ESP32 firmware, with the things every project needs already
decided: configuration, networking, MQTT, a command shell reachable over serial and a
browser, an HTTP management interface, and over-the-air updates with rollback.

Clone it, change the device name, and you have a device that joins a network, answers to
a name, publishes status, takes a new configuration over HTTP and can be updated without
a cable.

## What you get

| | |
|---|---|
| **Configuration** | One JSON Schema per project. The parser, the C types and the top-level walker are generated from it; there is no second description of the config to keep in step. Reads from an SD card if one is present, otherwise from flash. |
| **Shell** | `sys info`, `net status`, … over the serial port *and* a browser at the same time, with output from either appearing on both. |
| **Networking** | WiFi with prioritised known networks, reconnect backoff, and a configuration access point when nothing is joinable. mDNS, NTP and MQTT start themselves when a link appears. |
| **MQTT** | Topic-handler registry, retained status, last-will so subscribers know when the device drops, and the log forwarded to a topic. |
| **HTTP** | Status, read and replace the configuration, and firmware upload — with Basic auth decided per route. |
| **OTA** | A/B slots with rollback: firmware that does not boot cleanly undoes itself. |

## Getting started

```sh
git clone <this repo> my-project && cd my-project
git clone https://github.com/benvonhandorf/esp_components.git ../esp_components

cp config/config.example.json fs/config.json   # then edit it

idf.py set-target esp32s3
idf.py build
./tools/flash_a.py -p /dev/ttyACM0
```

Then rename the project in `CMakeLists.txt` (`project(idf_template)`) — it becomes the
firmware name, and OTA refuses an image built under a different one.

### Where the components come from

While `esp_components` is a local checkout, the root `CMakeLists.txt` finds it beside this
project, or wherever `ESP_COMPONENTS_DIR` points:

```sh
idf.py -DESP_COMPONENTS_DIR=/path/to/esp_components build
```

Once that repository is published, that block is replaced by version-pinned dependencies
in `main/idf_component.yml`. Nothing else changes — components are found by name either
way.

## Configuration

`config/config_schema.json` is the **one** description of this device's configuration.
Each section `$ref`s the schema owned by the component that consumes it:

```json
"wifi": { "$ref": "wifi_manager/wifi_manager_config_schema.json" },
"mqtt": { "$ref": "mqtt_manager/mqtt_manager_config_schema.json" }
```

From that, the build generates the C types, the parsers and a top-level walker that hands
each section to its owning component to parse **in place**. Adding a section without
wiring it up in `main/config_reader.c` does not compile.

Defaults live in the schemas, so a device with no `config.json` still boots — onto its own
access point, with the console running, ready to be configured. `fs/config.json` is
gitignored; it holds real credentials.

Replace it at runtime:

```sh
curl -u admin:secret -X POST --data-binary @fs/config.json http://device.local/api/config
```

It is parsed before it is stored, so a configuration that would not load cannot replace
one that does.

## Flashing, and why `flash_a.py`

`idf.py flash` writes `ota_0`, but the bootloader follows `otadata`. After an OTA the
device runs `ota_1`, so a plain flash writes the slot that is *not* selected and the
device comes back running the old firmware — silently. `tools/flash_a.py` erases `otadata`
as part of flashing.

## Updating over the air

```sh
./tools/ota_upload.py device.local build/idf_template.bin -u admin
```

The image header is checked after about a kilobyte, so the wrong binary is refused
quickly and by name. The new image boots on trial: `app_main()` confirms it only after
everything has started, so firmware that crashes during start-up is rolled back on the
next reset. Confirm or undo by hand with `sys confirm` and `sys rollback`.

## Layout

```
config/       config_schema.json -- the one authored description; and an example
fs/           contents of the LittleFS partition (config.json is gitignored)
main/         app_main, config_reader, and this project's console commands,
              HTTP routes and status message
partitions/   4 MB / 8 MB / 16 MB A/B layouts
tools/        flashing, OTA upload, status, MQTT tail
docs/         how to extend the template
```

`main/app_status.c` is the file to edit first: what a device reports is the one thing no
component can decide for you.

See [AGENTS.md](AGENTS.md) for the conventions this project follows and
[docs/](docs/README.md) for how to add a command, a route or a configuration section.
