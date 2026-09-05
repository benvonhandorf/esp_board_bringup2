#!/usr/bin/env python3
"""Check that the command reference and the firmware agree.

Every command registered in main/app_console.c must have a heading in the
docs page for its group, and every command a docs page names must exist.

This exists because the two drift silently and in the direction that matters
most: a message or a manual naming a command that was renamed is read by
someone who is already stuck.

    ./tools/check_docs.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Which docs page covers which group. A group with no page of its own is
# documented in another one -- the I2C parts all live in docs/i2c.md.
PAGE = {
    "sys": "sys", "net": "net", "wifi": "wifi",
    "gpio": "gpio", "gpio-pwm": "gpio",
    "i2c": "i2c", "i2c-ina219": "i2c", "i2c-ina226": "i2c", "i2c-ina237": "i2c",
    "i2c-sht4x": "i2c", "i2c-nau7802": "i2c", "i2c-lm75bdp": "i2c",
    "i2c-rx8130ce": "i2c", "i2c-aw9523b": "i2c", "i2c-pi4ioe": "i2c",
    "uart": "uart", "spi": "spi", "sd": "sd",
    "audio": "audio", "audio-nau8822": "audio", "audio-ns4168": "audio",
    "audio-sph0645": "audio",
    "touch": "touch", "loadcell": "loadcell",
    "board": "board", "board-cardputer": "board", "board-xiao": "board",
    "board-sensor": "board", "board-minstro": "board", "board-core-basic": "board",
}


def registered():
    src = (ROOT / "main" / "app_console.c").read_text()
    tables = {}
    for m in re.finditer(r"static const cli_command_t (\w+)\[\] = \{(.*?)\n\};", src, re.S):
        tables[m.group(1)] = re.findall(r'\{"([\w-]+)",', m.group(2))
    groups = {}
    for m in re.finditer(
        r"static const cli_group_t \w+ = \{\s*\.name = \"([\w-]+)\",.*?\.commands = (\w+),",
        src, re.S,
    ):
        groups[m.group(1)] = tables[m.group(2)]
    return groups


def main():
    groups = registered()
    problems = []

    for group, commands in sorted(groups.items()):
        page = PAGE.get(group)
        if page is None:
            problems.append(f"group '{group}' has no docs page assigned in check_docs.py")
            continue
        path = ROOT / "docs" / f"{page}.md"
        if not path.exists():
            problems.append(f"group '{group}' points at docs/{page}.md, which does not exist")
            continue
        text = path.read_text()
        headings = re.findall(r"^#+ +(.*)$", text, re.M)

        if group.startswith("board-"):
            # The board groups all take the same commands, so docs/board.md
            # documents them once under `board-<name> <subsystem>` and gives
            # each board a section naming the subsystems it has. Demanding a
            # heading per (board, command) pair would be eighteen near-identical
            # sections, which is a worse page, not a better-checked one.
            section = re.search(r"^### +%s\b(.*?)(?=^#|\Z)" % re.escape(group),
                                text, re.M | re.S)
            if not section:
                problems.append(f"{group}: no '### {group}' section in docs/{page}.md")
                continue
            for command in commands:
                if not re.search(r"`%s`" % re.escape(command), section.group(1)):
                    problems.append(
                        f"{group} {command}: not listed in the '{group}' section "
                        f"of docs/{page}.md")
            continue

        # A command is documented by a heading naming it, either bare
        # (`## `read ...``) or qualified (`## `i2c-sht4x read ...``).
        for command in commands:
            if not any(re.search(r"`(?:%s )?%s\b" % (re.escape(group), re.escape(command)), h)
                       for h in headings):
                problems.append(f"{group} {command}: no heading in docs/{page}.md")

    # And the other direction: a docs page must not name a command that is gone.
    valid = {f"{g} {c}" for g, cs in groups.items() for c in cs} | set(groups)
    for path in sorted((ROOT / "docs").glob("*.md")):
        text = path.read_text()
        for lit in re.findall(r"`([^`\n]+)`", text):
            words = lit.split()
            if (len(words) >= 2 and words[0] in groups
                    and not words[1].startswith("<") and words[1] != "..."):
                if f"{words[0]} {words[1]}" not in valid:
                    problems.append(f"docs/{path.name}: `{lit}` names no such command")

    if problems:
        for p in problems:
            print("ERR:", p)
        print(f"\n{len(problems)} problem(s)")
        return 1
    total = sum(len(c) for c in groups.values())
    print(f"ok: {total} commands in {len(groups)} groups, all documented")
    return 0


if __name__ == "__main__":
    sys.exit(main())
