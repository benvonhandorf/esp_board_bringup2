# Changelog

All notable changes to this component are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

## [0.1.0] - 2026-09-06

### Added

- `strres_compile.py`, which turns authored JSON sections into `.sr` blobs, the
  `STR_*` ids C names them by, and a catalogue naming each section's file.
- The reader: `strres_init`, `strres_printf`, `strres_error`, `strres_copy`,
  `strres_hold`/`strres_release`, `strres_set_locale`.
- `STRRES_PRINTF`/`STRRES_ERROR`, which keep `-Wformat` checking of the call site
  against the base locale without the text reaching the image.
- A section cache with least-recently-used eviction, a pinned section that is
  never evicted, and a hold refcount that protects a borrowed pointer.
- A catalogue hash carried by both the firmware and every blob, so resources
  built for a different set of ids fail visibly instead of printing the wrong
  sentence.
- `project_include.cmake` exporting `strres_generate()`, so a project generates
  its catalogue without defining anything itself.
- Host tests covering the blob format, lookup, misses, eviction, holds and
  locale fallback, run against fixtures built by the real compiler.
