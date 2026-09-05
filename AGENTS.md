# AGENTS.md

Orientation for anyone — human or agent — working in this project. Read this before
changing code; it should save most of a codebase scan.

## What this is

An ESP-IDF firmware project built from a template. The parts that every project needs —
configuration, WiFi, mDNS, NTP, MQTT, a command shell, HTTP, OTA — live in **shared
components** in a separate repository (`esp_components`), fetched by tag — see
`main/idf_component.yml`. What is *here* is the part specific to this device.

That split is the whole design. If you find yourself editing a shared component to make
this project work, ask first whether the component is missing a general capability or
whether the project is reaching through an abstraction it should be using.

ESP-IDF **v6.0.2**. C11, `-Wall -Wextra -Werror` on `main`.

## Layout

```
config/config_schema.json   the ONE description of this device's configuration
fs/                         LittleFS image contents; config.json is gitignored
main/
  main.c                    app_main: bring-up order, and the reasons for it
  config_reader.{c,h}       loads the config; dispatches sections to their owners
  app_status.{c,h}          what this device reports  <- edit this first
  app_console.{c,h}         this project's shell commands
  app_http.{c,h}            this project's HTTP routes
partitions/                 A/B OTA layouts for 4/8/16 MB
tools/                      flashing, OTA upload, status, MQTT tail
```

## Conventions

- **Never `printf()`.** Use `diag_printf()`, or output never reaches the web console.
  `ESP_LOGx` is already routed through the same fan-out.
- **Report command failures with `diag_error()`** — it prefixes `ERR:` so a host script
  can detect failure without parsing prose — and return `-1`; return `0` on success.
- **Parse arguments with the `cli_parse_*` helpers**, never `atoi()`. They must consume
  the whole token, so `gpio read foo` is an error rather than a read of pin 0.
- **`main` must never set `REQUIRES` or `PRIV_REQUIRES`.** `project.cmake` grants it an
  implicit dependency on every component in the build *only* while both are unset; naming
  one silently strips the rest.
- **A new file must be added to `SRCS` in `main/CMakeLists.txt`** or it is never compiled,
  silently.

## Adding things

**A configuration section.** Add a `$ref` to `config/config_schema.json` naming the
component that owns the schema, add the field to `app_config_full_t` and a
`SECTION_PARSER_<name>` line in `config_reader.c`. Forgetting the last step does not
compile: the generated `APP_CONFIG_SECTIONS` X-macro expands to something that needs it.

**A console command.** Add a `cli_command_t` row to a group in `app_console.c`. Commands
are `<group> <command>`; a command receives `argv[0] == its own name`. Deeper grouping is
a hyphenated group name (`i2c-nau7802`), not a third token.

**An HTTP route.** Add an `http_route_t` row in `app_http.c`. Set `require_auth` on
anything that exposes credentials or changes the device.

**Something this device measures.** `app_status.c`. Keep the JSON a flat object of
scalars — that is the shape that survives being graphed, alerted on and diffed.

## Things that will bite

- **`idf.py flash` is not enough.** It writes `ota_0` while the bootloader follows
  `otadata`, so after an OTA a plain flash leaves the device running the old firmware,
  silently. Use `tools/flash_a.py`.
- **OTA refuses an image from a differently-named project.** Renaming `project()` in
  `CMakeLists.txt` means the next OTA is rejected until the running firmware is one built
  under the new name. That is deliberate — uploading the wrong binary is the common
  accident — but it surprises people once.
- **The image is on trial after an OTA.** `app_main()` confirms it at the very end, so
  firmware that crashes during start-up rolls back on the next reset. Do not move that
  call earlier; it would confirm images that never worked.
- **`http_server` refuses to start with authentication enabled and no password.** That is
  not a bug. Set one in `config.json`.
- **A shared component is pinned to a git tag, not a version range.** Upgrading one is
  editing its `version:` in `main/idf_component.yml` to another tag; there is no range
  that picks up a newer release on its own. To edit a component and this project
  together, build with `-DESP_COMPONENTS_DIR=../esp_components` — a local component of
  that name wins over the fetched one, and the pin is then ignored for it. Anything
  changed that way has to be committed and tagged in `esp_components`, and the pin here
  moved to the new tag, before a plain `idf.py build` sees it.
- **A build with `ESP_COMPONENTS_DIR` set rewrites `dependencies.lock`**, replacing the
  git pins with local paths — the component manager records what it resolved, and there
  is no way to point it at a different lockfile. Committing that would un-pin the project
  for everyone. A plain `idf.py reconfigure` puts the pins back; check `git diff` before
  committing after working that way.
- **`dependencies.lock` is committed and target-specific.** It pins the managed component
  versions, so `idf.py set-target` rewrites it. That churn belongs in the commit; do not
  gitignore it, or two people get different component versions.
- **`sdkconfig` is generated and gitignored.** Edit `sdkconfig.defaults` or
  `sdkconfig.defaults.<target>`, then `idf.py reconfigure`.
- **`.vscode/settings.json` sets `IDF_TARGET` as an environment variable**, which outranks
  everything else. Switching targets without updating it fails with a CMake cache
  mismatch; build from a plain shell to avoid it.

## Verifying a change

```sh
idf.py build                       # -Werror is on for main
./tools/flash_a.py -p /dev/ttyACM0
```

On the device: `sys info` over serial, the same shell in a browser at the device's
address, `./tools/get_status.py <host>`, and an OTA round trip with
`./tools/ota_upload.py`.
