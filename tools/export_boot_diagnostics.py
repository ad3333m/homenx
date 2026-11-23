#!/usr/bin/env python3
"""Write a support report from a mounted HomeNX SD card."""
import argparse
from pathlib import Path
import re
import sys


def read_log(path):
    with path.open("rb") as stream:
        stream.seek(max(0, path.stat().st_size - 512 * 1024))
        return stream.read(512 * 1024).decode("utf-8", errors="replace").splitlines()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sd", type=Path, help="Mounted SD card root")
    args = parser.parse_args()
    config = args.sd / "config" / "SwitchU"
    print("HomeNX boot diagnostics")
    for component in ("daemon", "menu"):
        print(f"\n{component.upper()}")
        records = []
        for suffix in ("old", "log"):
            try:
                records.extend(read_log(config / f"diagnostic-{component}.{suffix}"))
            except FileNotFoundError:
                pass
            except OSError as error:
                print(f"Diagnostic read failed (errno {error.errno})")
        # A new process starts a new session. Preserve the final stage and Result
        # codes from the preceding session in the trace below.
        start = max((i for i, line in enumerate(records) if "__appInit BEGIN" in line), default=0)
        current = records[start:]
        stage = next((line for line in reversed(current)
                      if " BEGIN" in line or "frame submitted" in line), "Unavailable")
        failure = next((line for line in reversed(current) if "[ERROR]" in line), "Unavailable")
        print(f"Last recorded stage: {stage}")
        print(f"Last recorded error: {failure}")
        for line in current:
            if "/BOOT]" in line or "/MEM]" in line:
                print(line)
        if records:
            print("\nRecent diagnostic records")
            print("\n".join(records[-150:]))

    print("\nAtmosphere boot flags (configuration only)")
    root = args.sd / "atmosphere" / "contents"
    try:
        for path in sorted(root.iterdir()):
            if not path.is_dir() or not re.fullmatch(r"[0-9a-fA-F]{16}", path.name):
                continue
            active = (path / "flags" / "boot2.flag").is_file()
            parked = (path / "flags" / "boot2.homenx-disabled").is_file()
            legacy = (path / "boot2.flag").is_file()
            if active or parked or legacy or (path.name.upper() == "00FF0000636C6BFF" and
                                               (path / "exefs.nsp").is_file()):
                print(f"atmosphere/contents/{path.name} boot2={active} parked={parked} legacy={legacy}")
    except OSError as error:
        print(f"Sysmodule scan failed (errno {error.errno})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
