#!/usr/bin/env python3
"""
Download the last `audio pdmbits grab` from the device.

    ./tools/pdm_grab.py 192.168.1.50 grab.bin

The file is the raw receiver output: frames of little-endian uint16 words, one
word per slot (slot 0 then slot 1 for each data line), each word 16 PDM bits
with the oldest in the most significant bit -- the same words `pdmbits dump`
prints as hex. The channel count and nominal frame rate are printed, because
the file itself carries neither.
"""

import argparse
import getpass
import sys
import urllib.error
import urllib.request


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host", help="device hostname or address")
    ap.add_argument("output", help="file to write")
    ap.add_argument("-u", "--user", default="admin", help="HTTP username")
    ap.add_argument("-P", "--password", help="HTTP password; prompted if omitted")
    ap.add_argument("--port", type=int, default=80)
    args = ap.parse_args()

    password = args.password or getpass.getpass(f"Password for {args.user}: ")
    url = f"http://{args.host}:{args.port}/api/capture"
    manager = urllib.request.HTTPPasswordMgrWithDefaultRealm()
    manager.add_password(None, url, args.user, password)
    opener = urllib.request.build_opener(urllib.request.HTTPBasicAuthHandler(manager))

    try:
        with opener.open(url, timeout=60) as response, open(args.output, "wb") as out:
            channels = response.headers.get("X-Channels")
            rate = response.headers.get("X-Frames-Per-Second")
            total = 0
            while True:
                block = response.read(65536)
                if not block:
                    break
                out.write(block)
                total += len(block)
    except urllib.error.HTTPError as exc:
        print(f"{exc.code} {exc.reason}: {exc.read().decode(errors='replace')}",
              file=sys.stderr)
        return 1
    except urllib.error.URLError as exc:
        print(f"Could not reach {url}: {exc.reason}", file=sys.stderr)
        return 1

    print(f"{total} bytes -> {args.output}: {channels} channels, "
          f"{rate} frames/s nominal, uint16 little-endian")
    return 0


if __name__ == "__main__":
    sys.exit(main())
