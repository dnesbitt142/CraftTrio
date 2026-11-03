#!/usr/bin/env python3
"""Losslessly pack plain OpenWatcom LX pages with Warp's EXEPACK:2 codec."""
import argparse
import ctypes
import mmap
import os
from pathlib import Path
import struct
import subprocess
import tempfile

PAGE_ENTRY = struct.Struct("<IHH")


def integer(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def put_integer(data, offset, value):
    struct.pack_into("<I", data, offset, value)


def pack(source, output, codec):
    if source.resolve() == output.resolve():
        raise ValueError("Use a separate output file.")
    with source.open("rb") as stream, mmap.mmap(
        stream.fileno(), 0, access=mmap.ACCESS_READ
    ) as original:
        lx = integer(original, 0x3C)
        if original[:2] != b"MZ" or original[lx:lx + 4] != b"LX\0\0":
            raise ValueError("Expected a little-endian MZ/LX executable.")
        if integer(original, lx + 0x28) != 4096:
            raise ValueError("Expected 4096-byte LX pages.")
        if any(integer(original, lx + n) for n in (0x34, 0x3C, 0x4C, 0x7C, 0x90)):
            raise ValueError("Packed input and loader/page checksums are unsupported.")
        data = integer(original, lx + 0x80)
        shift = integer(original, lx + 0x2C)
        table = lx + integer(original, lx + 0x48)
        count = integer(original, lx + 0x14)
        if shift > 16 or not lx + 0xC4 <= table < table + 8 * count <= data <= len(original):
            raise ValueError("Invalid LX page table.")
        head = bytearray(original[:data])
        stored = ctypes.create_string_buffer(2 * 4096 + 2)
        packed_count = 0
        highest = data
        with tempfile.NamedTemporaryFile(dir=output.parent, delete=False) as temp:
            temp_path = Path(temp.name)
            try:
                temp.write(head)
                for i in range(count):
                    entry = table + i * 8
                    offset, size, flags = PAGE_ENTRY.unpack_from(original, entry)
                    if flags in (2, 3) and not size:
                        continue
                    start = data + (offset << shift)
                    if flags or size > 4096 or start + size > len(original):
                        raise ValueError(f"Unsupported LX page {i}.")
                    highest = max(highest, start + size)
                    page = original[start:start + size]
                    length = codec.lx_pack_page(page, size, stored)
                    if length:
                        if not codec.lx_verify_page(stored, length, page, size):
                            raise ValueError(f"Lossless packing check failed on page {i}.")
                        compressed = stored.raw[:length]
                        PAGE_ENTRY.pack_into(head, entry, temp.tell() - data, length, 5)
                        temp.write(compressed)
                        packed_count += 1
                    else:
                        PAGE_ENTRY.pack_into(head, entry, temp.tell() - data, size, flags)
                        temp.write(page)
                for field in (0x88, 0x98):
                    value = integer(head, lx + field)
                    if value:
                        if not highest <= value <= len(original):
                            raise ValueError("Unsupported LX trailer layout.")
                        put_integer(head, lx + field, value + temp.tell() - highest)
                temp.write(original[highest:])
                put_integer(head, lx + 0x2C, 0)
                if packed_count:
                    # Normal and iterated entries share the physical data area.
                    put_integer(head, lx + 0x4C, data)
                temp.seek(0)
                temp.write(head)
                temp.close()
                os.chmod(temp_path, source.stat().st_mode & 0o777)
                os.replace(temp_path, output)
            finally:
                temp_path.unlink(missing_ok=True)
    print(f"{output}: {packed_count:,} of {count:,} pages packed, "
          f"{source.stat().st_size:,} -> {output.stat().st_size:,} bytes")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    options = parser.parse_args()
    with tempfile.TemporaryDirectory(dir=options.output.parent) as build:
        library = Path(build).resolve() / "lx-page.so"
        subprocess.run([os.environ.get("CC", "cc"), "-O2", "-shared", "-fPIC",
                        "-o", library, Path(__file__).with_name("lx-page.c")],
                       check=True)
        codec = ctypes.CDLL(str(library))
        codec.lx_pack_page.argtypes = (ctypes.c_char_p, ctypes.c_uint, ctypes.c_void_p)
        codec.lx_pack_page.restype = ctypes.c_uint
        codec.lx_verify_page.argtypes = (ctypes.c_void_p, ctypes.c_uint, ctypes.c_char_p,
                                        ctypes.c_uint)
        codec.lx_verify_page.restype = ctypes.c_int
        pack(options.source, options.output, codec)


if __name__ == "__main__":
    main()
