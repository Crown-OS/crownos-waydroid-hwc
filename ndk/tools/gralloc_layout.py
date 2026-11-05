"""Checks the cros_gralloc_handle layout the HWC compiles against (x86_64 only).

1. Reads cros_gralloc_driver::allocate in the image's minigbm library and records
   which handle offset each drv_bo_get_*() result is stored at (the per-plane
   arrays and the plane count included), where the magic goes and how many
   ints native_handle_create() reserves.
2. Compiles static_asserts of those facts against the fork's minigbm header.
3. Checks the handle offsets the built HWC reads in create_buffer_cros /
   get_buffer_metadata_cros: each reader either reads exactly what the stock
   HWC reads (stock symbols come from its .gnu_debugdata mini debuginfo), or
   reads only fields step 2 proved. Stack accesses are not handle reads.

usage: python3 -I gralloc_layout.py <llvm-bin> <clang++> <minigbm-include> <cutils-include>
                                    <libminigbm.so> <stock-hwc.so> <built-hwc.so> <work-dir>
"""

import lzma
import re
import subprocess
import sys
from pathlib import Path

GETTER_FIELDS = {
    "drv_bo_get_width": "width",
    "drv_bo_get_height": "height",
    "drv_bo_get_format": "format",
    "drv_bo_get_tiling": "tiling",
    "drv_bo_get_format_modifier": "format_modifier",
    "drv_bo_get_use_flags": "use_flags",
    "drv_bo_get_pixel_stride": "pixel_stride",
    "drv_bo_get_total_size": "total_size",
}
PLANE_GETTER_FIELDS = {
    "drv_bo_get_plane_fd": "fds",
    "drv_bo_get_plane_stride": "strides",
    "drv_bo_get_plane_offset": "offsets",
    "drv_bo_get_plane_size": "sizes",
}
PLANE_ARRAYS = ("fds", "strides", "offsets", "sizes")
CROS_MAGIC = "0xabcddcba"
NATIVE_HANDLE_HEADER_INTS = 3
HWC_READERS = ("create_buffer_cros", "get_buffer_metadata_cros")
DISPLACEMENT = re.compile(r"\s(0x[0-9a-f]+)\(%(r\w+)(?:,%r\w+,\d)?\)")
STACK_REGISTERS = ("rsp", "rbp")
REGISTER_MOVE = re.compile(r"mov[lq]?\s+%(\w+), %(\w+)$")
STORE = re.compile(r"mov[lq]\s+%(\w+), (0x[0-9a-f]+)\(%r\w+(,%r\w+,4)?\)")


def base_register(name):
    """rax for eax/ax, r14 for r14d: the 64-bit register an operand names."""
    name = re.sub(r"[dwb]$", "", name) if re.match(r"r\d+[dwb]$", name) else name
    legacy = {"eax": "rax", "ebx": "rbx", "ecx": "rcx", "edx": "rdx", "esi": "rsi", "edi": "rdi",
              "ebp": "rbp", "esp": "rsp"}
    return legacy.get(name, name)


def run(*command):
    return subprocess.run(command, check=True, capture_output=True, text=True).stdout


def allocate_facts(llvm_bin, minigbm):
    listing = run(f"{llvm_bin}/llvm-objdump", "-d", "--no-show-raw-insn",
                  "--disassemble-symbols=_ZN19cros_gralloc_driver8allocateEPK30cros_gralloc_buffer_descriptorPP13native_handle",
                  str(minigbm)).splitlines()
    offsets, header_ints = {}, None
    pending, holders = None, set()
    for index, line in enumerate(listing):
        call = re.search(r"<(\w+)@plt>", line)
        if call:
            getter = call.group(1)
            if getter in GETTER_FIELDS or getter in PLANE_GETTER_FIELDS or getter == "drv_bo_get_num_planes":
                pending = GETTER_FIELDS.get(getter) or PLANE_GETTER_FIELDS.get(getter) or "num_planes"
                holders = {"rax"}
                continue
            holders.discard("rax")
            if getter == "native_handle_create":
                immediates = (re.search(r"\$(0x[0-9a-f]+), %esi", earlier) for earlier in reversed(listing[:index]))
                header_ints = int(next(m for m in immediates if m).group(1), 16)
        move = REGISTER_MOVE.search(line)
        if pending and move and base_register(move.group(1)) in holders:
            holders.add(base_register(move.group(2)))
        store = STORE.search(line)
        if pending and store and base_register(store.group(1)) in holders:
            indexed = bool(store.group(3))
            if indexed == (pending in PLANE_ARRAYS):
                offsets[pending] = int(store.group(2), 16)
                pending = None
        magic = re.search(rf"movl\s+\${CROS_MAGIC}, (0x[0-9a-f]+)\(%r\w+\)", line)
        if magic:
            offsets["magic"] = int(magic.group(1), 16)
    return offsets, header_ints


def proven_ranges(offsets):
    """[start, end) byte ranges of every proven field. A plane array ends
    where the next one starts (the probe asserts its sizeof), and the last
    one is as long as the one before it."""
    ranges = []
    arrays = [field for field in PLANE_ARRAYS if field in offsets]
    for field, offset in offsets.items():
        if field in PLANE_ARRAYS:
            position = arrays.index(field)
            if position + 1 < len(arrays):
                span = offsets[arrays[position + 1]] - offset
            else:
                span = offset - offsets[arrays[position - 1]]
            ranges.append((offset, offset + span))
        else:
            ranges.append((offset, offset + 4))
    return ranges


def static_assert_probe(clang, includes, offsets, header_ints, work_dir):
    lines = ["#include <cstddef>", "#include <cros_gralloc/cros_gralloc_handle.h>"]
    lines += [f'static_assert(offsetof(cros_gralloc_handle, {field}) == {offset}, "{field}");'
              for field, offset in sorted(offsets.items())]
    arrays = [field for field in PLANE_ARRAYS if field in offsets]
    for field, following in zip(arrays, arrays[1:]):
        lines.append(f'static_assert(sizeof(cros_gralloc_handle::{field}) == {offsets[following] - offsets[field]}, '
                     f'"{field} length");')
    lines.append("static_assert((sizeof(cros_gralloc_handle) - sizeof(native_handle_t)) / sizeof(int) == "
                 f'{header_ints}, "native_handle_create ints");')
    probe = Path(work_dir) / "cros_handle_probe.cpp"
    probe.write_text("\n".join(lines) + "\n")
    command = [clang, "--target=x86_64-linux-android33", "-std=c++17", "-fsyntax-only",
               "-Wno-invalid-offsetof", *[f"-I{d}" for d in includes], str(probe)]
    result = subprocess.run(command, capture_output=True, text=True)
    return result.returncode == 0, result.stderr


def function_ranges(llvm_bin, symbols_elf):
    ranges = {}
    for line in run(f"{llvm_bin}/llvm-nm", "-S", "-C", str(symbols_elf)).splitlines():
        parts = line.split(maxsplit=3)
        if len(parts) == 4:
            for reader in HWC_READERS:
                if parts[3].startswith(f"(anonymous namespace)::{reader}("):
                    ranges[reader] = (int(parts[0], 16), int(parts[1], 16))
    return ranges


def read_offsets(llvm_bin, binary, symbols_elf):
    reads = {}
    for reader, (start, size) in function_ranges(llvm_bin, symbols_elf).items():
        listing = run(f"{llvm_bin}/llvm-objdump", "-d", "--no-show-raw-insn",
                      f"--start-address={start:#x}", f"--stop-address={start + size:#x}", str(binary))
        reads[reader] = [m.group(1) for m in DISPLACEMENT.finditer(listing) if m.group(2) not in STACK_REGISTERS]
    return reads


def only_proven(reads, ranges):
    return all(any(start <= int(offset, 16) < end for start, end in ranges) for offset in reads)


def main(argv):
    if len(argv) != 9:
        print(__doc__, file=sys.stderr)
        return 2
    llvm_bin, clang, minigbm_include, cutils_include, minigbm, stock, built, work_dir = argv[1:]
    Path(work_dir).mkdir(parents=True, exist_ok=True)

    offsets, header_ints = allocate_facts(llvm_bin, minigbm)
    print(f"image minigbm allocate(): {header_ints} handle ints, offsets "
          + ", ".join(f"{k}={v:#x}" for k, v in sorted(offsets.items(), key=lambda kv: kv[1])))
    ok, errors = static_assert_probe(clang, (minigbm_include, cutils_include), offsets, header_ints, work_dir)
    print(f"fork header matches image allocator: {'yes' if ok else 'NO'}")
    if not ok:
        print(errors, file=sys.stderr)

    debugdata = Path(work_dir) / "stock-debugdata.elf"
    section = Path(work_dir) / "stock-debugdata.xz"
    run(f"{llvm_bin}/llvm-objcopy", f"--dump-section=.gnu_debugdata={section}", stock, str(Path(work_dir) / "stock.tmp"))
    debugdata.write_bytes(lzma.decompress(section.read_bytes()))
    stock_reads = read_offsets(llvm_bin, stock, debugdata)
    built_reads = read_offsets(llvm_bin, built, built)
    ranges = proven_ranges(offsets)
    reads_ok = len(built_reads) == len(HWC_READERS)
    for reader in HWC_READERS:
        stock_read, built_read = stock_reads.get(reader), built_reads.get(reader)
        verdict = ("as stock" if built_read == stock_read
                   else "proven fields only" if built_read and only_proven(built_read, ranges) else "UNPROVEN")
        reads_ok = reads_ok and verdict != "UNPROVEN"
        print(f"{reader}: stock {stock_read} built {built_read}: {verdict}")
    print(f"built HWC reads only handle fields the image allocator writes: {'yes' if reads_ok else 'NO'}")
    return 0 if ok and reads_ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
