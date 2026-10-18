#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""List the player's own symbols that the next steps of COLOR 2 and ROLL need.

The RX3 application is not stripped, so its symbol table names the classes
behind the mixer: where the Sound Color FX live, what selects one, where the
COLOR and PARAMETER values are kept, which LEDs light the buttons, the beat
grid and the Beat Jump distances. This reads that table from a binary you
already have and prints the matches, demangled when `c++filt` is installed.

It ships no firmware and embeds nothing from one. Point it at the `rbp` taken
from your own player's update, as REFERENCES.md describes:

    python3 scripts/find_fx_symbols.py path/to/rbp > fx-symbols.txt

The output is addresses and names only. Share it if you want help reading it;
never share the binary.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import shutil
import struct
import subprocess
import sys

# Mangled names keep their identifiers, so plain substrings find them.
TOPICS = {
    "sound color fx": r"colou?r|sndfx|soundfx|scfx|dubecho|dub_echo|crush|sweep|noise|space|filter",
    "mixer and channels": r"mixerengine|mixer|channelstrip|crossfader|talkover",
    "fx knobs and parameters": r"parameter|knob|adjust|level|depth",
    "leds and buttons": r"led|button|onkey|sendkey|physicalkey",
    "beat grid and tempo": r"beatgrid|beat_grid|grid|bpm|tempo|phase|quantize",
    "beat jump": r"beatjump|beat_jump|jump",
}


def symbols(binary: bytes):
    """(value, size, name) from .symtab and .dynsym of a 32-bit LE ELF."""
    if binary[:4] != b"\x7fELF" or binary[4] != 1 or binary[5] != 1:
        raise SystemExit("not a 32-bit little-endian ELF")
    shoff, = struct.unpack_from("<I", binary, 0x20)
    shentsize, shnum, _ = struct.unpack_from("<HHH", binary, 0x2E)
    sections = [struct.unpack_from("<IIIIIIIIII", binary, shoff + i * shentsize) for i in range(shnum)]
    found = []
    for name, kind, flags, addr, offset, size, link, info, align, entsize in sections:
        if kind not in (2, 11) or not entsize:          # SHT_SYMTAB, SHT_DYNSYM
            continue
        strings = sections[link]
        string_offset = strings[4]
        for at in range(offset, offset + size, entsize):
            st_name, st_value, st_size, st_info, _, _ = struct.unpack_from("<IIIBBH", binary, at)
            if not st_name or (st_info & 0xF) not in (1, 2):   # objects and functions
                continue
            end = binary.index(b"\0", string_offset + st_name)
            found.append((st_value, st_size, binary[string_offset + st_name:end].decode("latin-1")))
    return found


def demangle(names):
    tool = shutil.which("c++filt")
    if not tool or not names:
        return names
    result = subprocess.run([tool], input="\n".join(names), capture_output=True, text=True)
    lines = result.stdout.splitlines()
    return lines if len(lines) == len(names) else names


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("binary", type=pathlib.Path)
    arguments = parser.parse_args(argv)
    table = sorted(set(symbols(arguments.binary.read_bytes())))
    if not table:
        print("no symbols: this binary is stripped", file=sys.stderr)
        return 1
    readable = demangle([name for _, _, name in table])
    print(f"# {len(table)} symbols in {arguments.binary.name}")
    for topic, pattern in TOPICS.items():
        expression = re.compile(pattern, re.I)
        hits = [(value, size, pretty) for (value, size, raw), pretty in zip(table, readable)
                if expression.search(raw)]
        print(f"\n## {topic}: {len(hits)}")
        for value, size, pretty in hits:
            print(f"0x{value:08x} {size:6d} {pretty}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
