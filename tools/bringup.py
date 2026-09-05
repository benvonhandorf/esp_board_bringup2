#!/usr/bin/env python3
"""Drive the bringup console over the serial port.

Importable as a module by the test scripts, and usable on its own:

    ./bringup.py "sys info" "gpio survey"
    BRINGUP_PORT=/dev/ttyACM0 ./bringup.py "i2c scan"

The one piece of this worth reading is read_until_prompt(). The obvious way to
know a command has finished is to wait for the port to go quiet, and it does
not work here: `audio tone 1000 3` prints nothing at all for three seconds
while it plays, and a quiet-timeout gives up in the middle of it. Waiting for
the prompt instead is what makes the long-running commands drivable.
"""
import os
import sys
import time

try:
    import serial
except ImportError:                                          # pragma: no cover
    sys.exit("pyserial is needed: pip install pyserial")

DEFAULT_PORT = os.environ.get("BRINGUP_PORT", "/dev/ttyACM0")
BAUD = 115200
PROMPT = b">"


class Console:
    """A connection to the console, one command at a time."""

    def __init__(self, port=DEFAULT_PORT, baud=BAUD, settle=0.4):
        self.serial = serial.Serial(port, baud, timeout=0.1)
        time.sleep(settle)
        self.serial.reset_input_buffer()
        # Wake the prompt so boot chatter lands before the first command.
        self.serial.write(b"\r\n")
        self.banner = self.read_until_prompt(idle=1.0, limit=8.0)

    def read_until_prompt(self, idle=0.3, limit=60.0):
        out = bytearray()
        last = start = time.time()
        while time.time() - start < limit:
            waiting = self.serial.in_waiting
            if waiting:
                out += self.serial.read(waiting)
                last = time.time()
            elif out and time.time() - last > idle and out.rstrip().endswith(PROMPT):
                break
            else:
                time.sleep(0.02)
        return out.decode("utf-8", "replace")

    def send(self, command, idle=0.3, limit=60.0):
        """Run one command and return everything it printed."""
        self.serial.write(command.encode() + b"\r\n")
        return self.read_until_prompt(idle=idle, limit=limit)

    def close(self):
        self.serial.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


class Checks:
    """Pass/fail bookkeeping shared by the hardware tests."""

    def __init__(self, title=""):
        self.failures = []
        self.passes = 0
        if title:
            print(f"== {title}")

    def section(self, name):
        print(f"\n{name}")

    def equal(self, label, got, want, fmt="{:#05x}"):
        ok = got == want
        shown = "none" if got is None else fmt.format(got)
        print(f"  {label:<38} {shown}  want {fmt.format(want)}  "
              f"{'ok' if ok else 'FAIL'}")
        self._record(label, ok)
        return ok

    def truth(self, label, ok, detail=""):
        print(f"  {label:<38} {detail}  {'ok' if ok else 'FAIL'}")
        self._record(label, ok)
        return ok

    def _record(self, label, ok):
        if ok:
            self.passes += 1
        else:
            self.failures.append(label)

    def report(self):
        print()
        if self.failures:
            print(f"{self.passes} passed, {len(self.failures)} FAILED: "
                  + ", ".join(self.failures))
            return 1
        print(f"all {self.passes} checks passed")
        return 0


def main():
    commands = sys.argv[1:]
    if not commands:
        sys.exit(__doc__)
    with Console() as console:
        for command in commands:
            print(f"\n===== {command} =====")
            print(console.send(command), end="")


if __name__ == "__main__":
    main()
