#!/usr/bin/env python3
"""Fail the build when static RAM leaves too little startup heap to boot.

On pre-v3 ESP32-P4 silicon (the JC8012P4A1 ships with rev 1.x), ESP-IDF's
heap before its `main` task runs is only the low internal RAM region
(0x4ff00000..0x4ff3afc0) minus the static data linked into it. That data holds
every ESPHome entity and global. IDF's startup allocations and then the 8 KB
main task stack must fit in what remains; if they do not, the panel aborts at
power-up in app_startup.c (`assert(res == pdTRUE)`) before ESPHome runs, and an
OTA install is rolled back. That is how the 8-thermostat builds (#63, #73)
failed on the panel while compiling cleanly in CI.

The startup heap is fixed at link time: 0x4ff3afc0 - _heap_start_low. The
4-thermostat build (#72) booted with 111488 bytes and #73 aborted with 107008
(both ESPHome 2026.10.2). This check fails below the known-good figure, so a
change has to make room before it can add static RAM. See CLAUDE.md, Gotchas
("Static internal RAM") and components/calendar_json/alloc_diag.h.

Usage: check_startup_ram.py <build dir to search for firmware.elf>
"""

import glob
import os
import subprocess
import sys

LOW_DRAM_END = 0x4FF3AFC0
KNOWN_GOOD = 111488  # bytes, #72 on ESPHome 2026.10.2


def find_nm(elf):
    # The build records the toolchain it used: CMAKE_NM in the CMakeCache.txt
    # next to the ELF. Where that toolchain lives depends on the ESPHome version
    # and OS (~/.platformio before the native ESP-IDF build, the ESPHome cache
    # directory after), so do not guess when the build can say.
    cache = os.path.join(os.path.dirname(elf), "CMakeCache.txt")
    if os.path.exists(cache):
        with open(cache, encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("CMAKE_NM:"):
                    nm = line.split("=", 1)[1].strip()
                    if os.path.exists(nm):
                        return nm
    patterns = [
        "~/.platformio/packages/**/riscv32-esp-elf-nm",
        "~/.cache/esphome/**/riscv32-esp-elf-nm",
        "~/Library/Caches/esphome/**/riscv32-esp-elf-nm",
        "~/.espressif/**/riscv32-esp-elf-nm",
    ]
    for pattern in patterns:
        hits = glob.glob(os.path.expanduser(pattern), recursive=True)
        if hits:
            return hits[0]
    sys.exit(f"riscv32-esp-elf-nm not found (no CMAKE_NM in {cache}, none in {', '.join(patterns)})")


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "builds/.esphome/build"
    elfs = [p for p in glob.glob(os.path.join(root, "**", "firmware.elf"), recursive=True)]
    if not elfs:
        sys.exit(f"no firmware.elf under {root}")
    elf = max(elfs, key=os.path.getmtime)
    out = subprocess.run([find_nm(elf), elf], capture_output=True, text=True, check=True).stdout
    syms = {line.split()[-1]: int(line.split()[0], 16) for line in out.splitlines() if len(line.split()) == 3}
    if "_heap_start_low" not in syms:
        print(f"{elf}: no _heap_start_low (not a pre-v3 ESP32-P4 layout); skipping")
        return
    heap = LOW_DRAM_END - syms["_heap_start_low"]
    print(f"{elf}: startup heap (low internal RAM) {heap} bytes, known to boot {KNOWN_GOOD} "
          f"({heap - KNOWN_GOOD:+d})")
    if heap < KNOWN_GOOD:
        sys.exit(
            "Static internal RAM has grown past the last build known to boot on the panel. "
            "Move state to PSRAM or shrink something else first (CLAUDE.md, Gotchas, "
            '"Static internal RAM").'
        )


if __name__ == "__main__":
    main()
