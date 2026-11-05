"""Compares the ABI of a rebuilt HIDL interface library with the image's copy:
the set of strong exported symbols and, slot by slot, every exported vtable,
construction vtable and VTT. A slot is named by the symbol its relocation
points at, so two libraries match only if each virtual method sits at the same
index, which is what the HWC's generated headers rely on.

usage: python3 -I hidl_abi.py <llvm-readelf> <rebuilt.so> <image.so>
"""

import importlib.util
import re
import struct
import subprocess
import sys
from bisect import bisect_right
from pathlib import Path

spec = importlib.util.spec_from_file_location("elf_symbols", Path(__file__).with_name("elf_symbols.py"))
elf_symbols = importlib.util.module_from_spec(spec)
spec.loader.exec_module(elf_symbols)

PT_LOAD = 1
POINTER_SIZE = 8
VTABLE_PREFIXES = ("_ZTV", "_ZTC", "_ZTT")
RELOCATION = re.compile(r"^([0-9a-f]{16})\s+[0-9a-f]{16}\s+(R_\w+)(?:\s+[0-9a-f]{16}\s+(\S+)\s+\+\s+([0-9a-f]+))?")


class Image:
    def __init__(self, readelf, path):
        self.library = elf_symbols.parse(path)
        self.data = Path(path).read_bytes()
        phoff, = struct.unpack_from("<Q", self.data, 0x20)
        phentsize, phnum = struct.unpack_from("<HH", self.data, 0x36)
        self.segments = []
        for index in range(phnum):
            kind, _flags, offset, vaddr, _paddr, filesz = struct.unpack_from("<IIQQQQ", self.data, phoff + index * phentsize)
            if kind == PT_LOAD:
                self.segments.append((vaddr, offset, filesz))
        self.symbolic = {}
        listing = subprocess.run([readelf, "-r", "-W", str(path)], check=True, capture_output=True, text=True).stdout
        for line in listing.splitlines():
            match = RELOCATION.match(line)
            if match and match.group(3):
                addend = int(match.group(4), 16)
                self.symbolic[int(match.group(1), 16)] = match.group(3) + (f"+{addend:#x}" if addend else "")
        defined = sorted((s.value, s.size, s.name) for s in self.library.symbols if s.defined and s.value)
        self.starts = [value for value, _, _ in defined]
        self.defined = defined

    def word(self, address):
        for vaddr, offset, filesz in self.segments:
            if vaddr <= address < vaddr + filesz:
                return struct.unpack_from("<Q", self.data, offset + address - vaddr)[0]
        return 0

    def name_of(self, address):
        index = bisect_right(self.starts, address) - 1
        if index < 0 or address == 0:
            return f"{address:#x}"
        value, size, name = self.defined[index]
        if address == value:
            return name
        return f"{name}+{address - value:#x}" if address < value + max(size, 1) else "<local>"

    def slots(self, symbol):
        names = []
        for address in range(symbol.value, symbol.value + symbol.size, POINTER_SIZE):
            names.append(self.symbolic.get(address) or self.name_of(self.word(address)))
        return names

    def strong_exports(self):
        return {s.name for s in self.library.symbols if s.defined and not s.weak}

    def vtables(self):
        return {s.name: s for s in self.library.symbols if s.defined and s.name.startswith(VTABLE_PREFIXES)}


def main(argv):
    if len(argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    rebuilt, image = Image(argv[1], argv[2]), Image(argv[1], argv[3])
    problems = []
    for title, names in (("only in rebuilt", rebuilt.strong_exports() - image.strong_exports()),
                         ("only in image", image.strong_exports() - rebuilt.strong_exports())):
        problems.extend(f"{title}: {name}" for name in sorted(names))

    rebuilt_vtables, image_vtables = rebuilt.vtables(), image.vtables()
    for name in sorted(rebuilt_vtables.keys() & image_vtables.keys()):
        ours, theirs = rebuilt.slots(rebuilt_vtables[name]), image.slots(image_vtables[name])
        if ours != theirs:
            first = next((i for i, (a, b) in enumerate(zip(ours, theirs)) if a != b), min(len(ours), len(theirs)))
            problems.append(f"{name}: {len(ours)} vs {len(theirs)} slots, first difference at slot {first}")
    print(f"{Path(argv[3]).name}: {len(rebuilt.strong_exports())} strong exports, "
          f"{len(rebuilt_vtables.keys() & image_vtables.keys())} vtables compared, {len(problems)} differences")
    for problem in problems:
        print(f"  {problem}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
