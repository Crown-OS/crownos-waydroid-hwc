"""Resolves every dynamic import of an ELF64 shared object against a sysroot the
way bionic's linker does: breadth-first over DT_NEEDED, honouring GNU symbol
versions, rejecting hidden definitions for unversioned references.

usage: python3 -I elf_symbols.py <library.so> <sysroot-libdir> [stock.so]
Exit status 1 when a strong import is unresolved, a version requirement names a
file that is not a direct DT_NEEDED, or any symbol uses the NDK's std::__ndk1.
"""

import struct
import sys
from collections import Counter, deque
from dataclasses import dataclass, field
from pathlib import Path

SHT_DYNAMIC = 6
SHT_DYNSYM = 11
SHT_GNU_VERDEF = 0x6FFFFFFD
SHT_GNU_VERNEED = 0x6FFFFFFE
SHT_GNU_VERSYM = 0x6FFFFFFF
DT_NEEDED = 1
STB_WEAK = 2
VERSYM_HIDDEN = 0x8000
VERSYM_GLOBAL = 1
NDK_LIBCXX_NAMESPACE = "__ndk1"


@dataclass
class Symbol:
    name: str
    defined: bool
    weak: bool
    version_index: int
    value: int = 0
    size: int = 0


@dataclass
class SharedObject:
    path: Path
    needed: list = field(default_factory=list)
    symbols: list = field(default_factory=list)
    has_versym: bool = False
    verdef: dict = field(default_factory=dict)
    verneed: dict = field(default_factory=dict)

    def definitions(self):
        return {s.name: s for s in self.symbols if s.defined}


def read_cstring(blob, offset):
    return blob[offset:blob.index(b"\0", offset)].decode()


def parse(path):
    data = Path(path).read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        raise ValueError(f"{path}: not a little-endian ELF64 file")
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize) for i in range(shnum)]
    by_type = {}
    for section in sections:
        by_type.setdefault(section[1], section)

    def body(section):
        return data[section[4]:section[4] + section[5]]

    dynsym = by_type[SHT_DYNSYM]
    strtab = body(sections[dynsym[6]])
    library = SharedObject(Path(path))

    for offset in range(0, len(body(by_type[SHT_DYNAMIC])), 16):
        tag, value = struct.unpack_from("<qQ", body(by_type[SHT_DYNAMIC]), offset)
        if tag == DT_NEEDED:
            library.needed.append(read_cstring(strtab, value))

    versym = body(by_type[SHT_GNU_VERSYM]) if SHT_GNU_VERSYM in by_type else b""
    library.has_versym = bool(versym)
    symtab = body(dynsym)
    for index in range(1, len(symtab) // 24):
        name, info, _other, shndx, value, size = struct.unpack_from("<IBBHQQ", symtab, index * 24)
        version = struct.unpack_from("<H", versym, index * 2)[0] if versym else 0
        library.symbols.append(
            Symbol(read_cstring(strtab, name), shndx != 0, info >> 4 == STB_WEAK, version, value, size))

    if SHT_GNU_VERDEF in by_type:
        blob, offset = body(by_type[SHT_GNU_VERDEF]), 0
        while True:
            _v, _flags, ndx, _cnt, _hash, aux, next_ = struct.unpack_from("<HHHHIII", blob, offset)
            library.verdef[read_cstring(strtab, struct.unpack_from("<I", blob, offset + aux)[0])] = ndx
            if not next_:
                break
            offset += next_
    if SHT_GNU_VERNEED in by_type:
        blob, offset = body(by_type[SHT_GNU_VERNEED]), 0
        while True:
            _v, count, file_, aux, next_ = struct.unpack_from("<HHIII", blob, offset)
            aux_offset = offset + aux
            for _ in range(count):
                _hash, _flags, other, name, aux_next = struct.unpack_from("<IHHII", blob, aux_offset)
                library.verneed[other] = (read_cstring(strtab, file_), read_cstring(strtab, name))
                aux_offset += aux_next
            if not next_:
                break
            offset += next_
    return library


def load_group(root, libdir):
    group, queue, seen = [], deque(root.needed), set()
    while queue:
        name = queue.popleft()
        if name in seen:
            continue
        seen.add(name)
        path = libdir / name
        if not path.exists():
            raise FileNotFoundError(f"DT_NEEDED {name} is not in {libdir}")
        library = parse(path)
        group.append(library)
        queue.extend(library.needed)
    return group


def accepts(provider, definition, required_version):
    if not provider.has_versym:
        return True
    actual = definition.version_index & ~VERSYM_HIDDEN
    if required_version is None:
        return not definition.version_index & VERSYM_HIDDEN
    return actual == provider.verdef.get(required_version, VERSYM_GLOBAL)


def resolve(root, group):
    definitions = [(library, library.definitions()) for library in group]
    results, errors = [], []
    for symbol in (s for s in root.symbols if not s.defined):
        file_, version = root.verneed.get(symbol.version_index, (None, None))
        label = f"{symbol.name}@{version}" if version else symbol.name
        if file_ and file_ not in root.needed:
            errors.append(f"{label}: version file {file_} is not a direct DT_NEEDED")
        provider = next((lib for lib, defs in definitions
                         if symbol.name in defs and accepts(lib, defs[symbol.name], version)), None)
        if provider is None:
            if symbol.weak:
                results.append((label, "<weak, unresolved>"))
            else:
                errors.append(f"{label}: unresolved")
            continue
        if file_ and provider.path.name != file_:
            errors.append(f"{label}: resolves to {provider.path.name}, version file is {file_}")
        results.append((label, provider.path.name))
    errors.extend(f"{s.name}: NDK libc++ namespace" for s in root.symbols if NDK_LIBCXX_NAMESPACE in s.name)
    return results, errors


def import_labels(library):
    labels = set()
    for symbol in (s for s in library.symbols if not s.defined):
        version = library.verneed.get(symbol.version_index, (None, None))[1]
        labels.add(f"{symbol.name}@{version}" if version else symbol.name)
    return labels


def main(argv):
    if len(argv) not in (3, 4):
        print(__doc__, file=sys.stderr)
        return 2
    root = parse(argv[1])
    results, errors = resolve(root, load_group(root, Path(argv[2])))

    report = Path(argv[1]).with_suffix(".imports.tsv")
    report.write_text("".join(f"{label}\t{provider}\n" for label, provider in sorted(results)))
    cxx_imports = sum("St3__1" in label for label, _ in results)
    print(f"{len(results)} imports resolved, {len(errors)} errors; "
          f"{cxx_imports} use std::__1; table: {report}")
    for provider, count in sorted(Counter(p for _, p in results).items()):
        print(f"  {count:4d}  {provider}")

    if len(argv) == 4:
        built, stock = import_labels(root), import_labels(parse(argv[3]))
        for title, names in (("only in stock", stock - built), ("only in built", built - stock)):
            print(f"{title} ({len(names)}):")
            for name in sorted(names):
                print(f"    {name}")

    for error in errors:
        print(f"ERROR {error}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
