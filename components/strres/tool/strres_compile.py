#!/usr/bin/env python3
"""
Compile authored JSON string catalogues into the .sr blobs the strres runtime
reads, plus the generated header and catalogue that name them from C.

The point of the exercise is flash: a string in the application image occupies
the OTA slot, and is duplicated across both slots, while a string on the `res`
partition costs one copy of one partition that OTA does not touch. So nothing
here may put the text back into the binary. Call sites reference a uint16 id;
the only strings this emits into C are the section file names.

Authoring format, one file per section:

    {
      "section": "i2c",
      "strings": {
        "bus.help": "Initialize the I2C bus on the given pins",
        "no_bus":   { "text": "No I2C bus. Run 'i2c bus <scl> <sda>' first.",
                      "pin": true }
      }
    }

A value is either the string itself or an object carrying `text` and an
optional `pin`. Pinned strings are moved into section 0, which the runtime
loads once and never evicts.
"""

import argparse
import json
import re
import sys
from pathlib import Path

MAGIC = b"SR01"
PIN_SECTION = "_pin"

# uint16 ids, split 6 bits of section and 10 bits of index.
SECTION_BITS = 6
INDEX_BITS = 10
MAX_SECTIONS = 1 << SECTION_BITS
MAX_STRINGS = 1 << INDEX_BITS

# A conversion specifier, less the %% escape which consumes no argument.
SPEC_RE = re.compile(
    r"%[-+ #0']*[0-9*]*(?:\.[0-9*]+)?(?:hh|h|ll|l|z|j|t|L)?([diouxXeEfFgGaAcspn])")


class Error(Exception):
    pass


def specifiers(text):
    """The conversion specifiers in `text`, in order, as a comparable list.

    Two strings that agree here take the same arguments, which is the property a
    translation has to preserve: STRRES_PRINTF checks the call site against the
    base locale at compile time, so a locale that disagrees would fault at
    runtime with no warning anywhere.
    """
    return SPEC_RE.findall(text.replace("%%", ""))


def ident(name):
    """`i2c-nau7802` / `scan.help` -> `I2C_NAU7802` / `SCAN_HELP`."""
    out = re.sub(r"[^0-9A-Za-z]+", "_", name).strip("_").upper()
    if not out or out[0].isdigit():
        raise Error("cannot form a C identifier from %r" % name)
    return out


def fnv1a(data):
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def load_section(path):
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        raise Error("%s: %s" % (path, e)) from None
    if not isinstance(doc, dict):
        raise Error("%s: top level must be an object" % path)

    section = doc.get("section", path.stem)
    if not isinstance(section, str) or not section:
        raise Error("%s: 'section' must be a non-empty string" % path)
    if section == PIN_SECTION:
        raise Error("%s: '%s' is reserved for pinned strings" % (path, PIN_SECTION))

    strings = doc.get("strings")
    if not isinstance(strings, dict):
        raise Error("%s: 'strings' must be an object" % path)

    out = {}
    for key, value in strings.items():
        if isinstance(value, str):
            text, pin = value, False
        elif isinstance(value, dict):
            if "text" not in value:
                raise Error("%s: %s/%s has no 'text'" % (path, section, key))
            text, pin = value["text"], bool(value.get("pin", False))
            if not isinstance(text, str):
                raise Error("%s: %s/%s 'text' must be a string" % (path, section, key))
        else:
            raise Error("%s: %s/%s must be a string or an object" % (path, section, key))
        out[key] = (text, pin)
    return section, out


def read_locale(locale_dir):
    """{section: {key: (text, pin)}} for one locale directory."""
    sections = {}
    for path in sorted(locale_dir.glob("*.json")):
        section, strings = load_section(path)
        if section in sections:
            raise Error("%s: section %r declared twice" % (locale_dir, section))
        sections[section] = strings
    return sections


def assign(base):
    """Work out the id of every string from the base locale.

    Sections and keys are both sorted, so an id depends only on the set of names
    -- not on authoring order, and not on the text. Section 0 is the pinned
    section and its index 0 is reserved, which is what makes id 0 mean "unset"
    for a zero-initialised struct field.

    Returns (ids, layout): ids maps (section, key) -> id, and layout maps a
    section *file* name -> the ordered list of (section, key) it stores.
    """
    pinned = [(s, k) for s in sorted(base) for k in sorted(base[s]) if base[s][k][1]]
    if len(pinned) + 1 > MAX_STRINGS:
        raise Error("%d pinned strings exceeds the %d limit" % (len(pinned), MAX_STRINGS - 1))

    names = sorted(base)
    if len(names) + 1 > MAX_SECTIONS:
        raise Error("%d sections exceeds the %d limit" % (len(names) + 1, MAX_SECTIONS))

    ids = {}
    layout = {PIN_SECTION: [None]}      # index 0 reserved, so id 0 is never valid
    for index, ref in enumerate(pinned, start=1):
        ids[ref] = index
        layout[PIN_SECTION].append(ref)

    for number, section in enumerate(names, start=1):
        keys = [k for k in sorted(base[section]) if not base[section][k][1]]
        if len(keys) > MAX_STRINGS:
            raise Error("section %r has %d strings, over %d"
                        % (section, len(keys), MAX_STRINGS))
        layout[section] = []
        for index, key in enumerate(keys):
            ids[(section, key)] = (number << INDEX_BITS) | index
            layout[section].append((section, key))
    return ids, layout


def catalog_hash(ids):
    """A hash of the id assignment, deliberately not of the text.

    Firmware carries this and every .sr repeats it, so a mismatch is caught at
    init. Hashing only the structure means correcting a typo does not strand a
    device whose `res` partition is a build behind -- it shows the old wording,
    which is stale but true. Adding or removing a string does shift indices, and
    that is exactly when the resources must be reflashed.
    """
    canon = "\n".join("%04x %s/%s" % (i, s, k)
                      for (s, k), i in sorted(ids.items(), key=lambda kv: kv[1]))
    return fnv1a(canon.encode("utf-8"))


def pack(entries, digest):
    """One .sr blob. entries is the ordered list of strings, holes as ""."""
    blob = bytearray()
    offsets = []
    for text in entries:
        offsets.append(len(blob))
        blob += (text or "").encode("utf-8") + b"\0"
    offsets.append(len(blob))           # sentinel, so the last length is known
    if len(blob) > 0xFFFF:
        raise Error("section blob is %d bytes, over the 65535 the offsets allow" % len(blob))

    out = bytearray(MAGIC)
    out += digest.to_bytes(4, "little")
    out += len(entries).to_bytes(2, "little")
    out += (0).to_bytes(2, "little")    # reserved
    for off in offsets:
        out += off.to_bytes(2, "little")
    out += blob
    return bytes(out)


def check_locale(locale, sections, base, base_name):
    """A translation must agree with the base on keys and on specifiers."""
    problems = []
    for section, strings in sections.items():
        if section not in base:
            problems.append("%s: section %r is not in %s" % (locale, section, base_name))
            continue
        for key, (text, _pin) in strings.items():
            if key not in base[section]:
                problems.append("%s: %s/%s is not in %s" % (locale, section, key, base_name))
                continue
            want = specifiers(base[section][key][0])
            got = specifiers(text)
            if want != got:
                problems.append(
                    "%s: %s/%s takes %s but %s takes %s -- the call site is checked "
                    "against %s, so this would fault at runtime"
                    % (locale, section, key, got or "no arguments", base_name,
                       want or "no arguments", base_name))
    return problems


def emit_header(path, ids, digest, base):
    lines = [
        "/*",
        " * Generated by strres_compile.py -- do not edit.",
        " *",
        " * Each STR_* is a uint16 id, so naming a string costs nothing in the image.",
        " * The STRRES_FMT_* beside it exists only for the dead branch inside",
        " * STRRES_PRINTF, which is how a call site keeps its -Wformat check without",
        " * the text ever reaching .rodata.",
        " */",
        "#ifndef STRRES_IDS_H",
        "#define STRRES_IDS_H",
        "",
        '#include "strres.h"',
        "",
        "#define STRRES_CATALOG_HASH 0x%08Xu" % digest,
        "#define STRRES_SECTION_COUNT %d" % (len(base) + 1),
        "",
        "/* The catalogue naming each section's file. Pass it to strres_init(). */",
        "extern const strres_catalog_t strres_generated_catalog;",
        "",
    ]
    for (section, key), value in sorted(ids.items(), key=lambda kv: kv[1]):
        name = "STR_%s_%s" % (ident(section), ident(key))
        lines.append("#define %s ((strres_id_t)0x%04X)" % (name, value))
        lines.append("#define STRRES_FMT_%s %s" % (name, json.dumps(base[section][key][0])))
    lines += ["", "#endif /* STRRES_IDS_H */", ""]
    path.write_text("\n".join(lines), encoding="utf-8")


def emit_catalog(path, digest, base):
    names = [PIN_SECTION] + sorted(base)
    lines = [
        "/* Generated by strres_compile.py -- do not edit. */",
        '#include "strres.h"',
        "",
        "static const char *const s_sections[] = {",
    ]
    lines += ['    "%s",' % n for n in names]
    lines += [
        "};",
        "",
        "const strres_catalog_t strres_generated_catalog = {",
        "    .hash = 0x%08Xu," % digest,
        "    .section_count = %d," % len(names),
        "    .section_names = s_sections,",
        "};",
        "",
    ]
    path.write_text("\n".join(lines), encoding="utf-8")


def emit_map(path, ids):
    lines = ["# id\tsection/key -- decodes a [str:XXXX] seen on a device"]
    lines += ["%04X\t%s/%s" % (i, s, k)
              for (s, k), i in sorted(ids.items(), key=lambda kv: kv[1])]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", type=Path,
                    help="directory holding one subdirectory per locale")
    ap.add_argument("--base-locale", default="en-US")
    ap.add_argument("--out-fs", type=Path, required=True,
                    help="directory to write <locale>/<section>.sr into")
    ap.add_argument("--out-header", type=Path, required=True)
    ap.add_argument("--out-catalog", type=Path, required=True)
    ap.add_argument("--out-map", type=Path)
    args = ap.parse_args(argv)

    base_dir = args.source / args.base_locale
    if not base_dir.is_dir():
        raise Error("no base locale at %s" % base_dir)

    base = read_locale(base_dir)
    if not base:
        raise Error("%s holds no .json section files" % base_dir)

    ids, layout = assign(base)
    digest = catalog_hash(ids)

    locales = sorted(p for p in args.source.iterdir() if p.is_dir())
    problems = []
    written = 0
    for locale_dir in locales:
        locale = locale_dir.name
        sections = base if locale == args.base_locale else read_locale(locale_dir)
        if locale != args.base_locale:
            problems += check_locale(locale, sections, base, args.base_locale)
            if problems:
                continue

        out_dir = args.out_fs / locale
        out_dir.mkdir(parents=True, exist_ok=True)
        for file_name, refs in layout.items():
            entries = []
            for ref in refs:
                if ref is None:
                    entries.append("")      # the reserved id 0
                    continue
                section, key = ref
                # Fall back to the base locale for anything untranslated, so a
                # partial translation reads as mixed rather than as missing.
                pair = sections.get(section, {}).get(key) or base[section][key]
                entries.append(pair[0])
            (out_dir / ("%s.sr" % file_name)).write_bytes(pack(entries, digest))
            written += 1

    if problems:
        for p in problems:
            print("strres: %s" % p, file=sys.stderr)
        raise Error("%d problem(s) in translations" % len(problems))

    args.out_header.parent.mkdir(parents=True, exist_ok=True)
    emit_header(args.out_header, ids, digest, base)
    emit_catalog(args.out_catalog, digest, base)
    if args.out_map:
        emit_map(args.out_map, ids)

    print("strres: %d strings in %d sections, %d locale(s), %d file(s), catalog 0x%08X"
          % (len(ids), len(base) + 1, len(locales), written, digest))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Error as e:
        print("strres: %s" % e, file=sys.stderr)
        sys.exit(1)
