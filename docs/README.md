# docs/

How to extend this project. The shared components document themselves in
`esp_components/<component>/README.md`; this covers what belongs *here*.

- [Configuration](configuration.md) — how one schema becomes the parsers, and how to add
  a section.
- [Bring-up order](bringup.md) — what `app_main()` does and why the order is what it is.
- [Updating firmware](ota.md) — flashing, OTA, rollback, and the failure modes.

Keep documentation beside the code and change it in the same commit. That is the reason
it lives in the repository rather than a wiki.
