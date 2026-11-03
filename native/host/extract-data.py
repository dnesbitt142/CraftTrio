#!/usr/bin/env python3
"""Place wasm2c's generated constant byte arrays in a 32-bit Intel OMF object."""
import argparse
import re
import struct
from pathlib import Path


ARRAY = re.compile(rb"const u8 (data_segment_data_[A-Za-z0-9_]+)\[\] = \{\r?\n")


def record(output, kind, payload):
    header = struct.pack("<BH", kind, len(payload) + 1)
    checksum = -(sum(header) + sum(payload)) & 0xFF
    output.write(header + payload + bytes([checksum]))


def name(value):
    if not 0 < len(value) <= 255:
        raise ValueError("Invalid OMF name")
    return bytes([len(value)]) + value


def collect(source, rewritten, prefix):
    data = bytearray()
    symbols = []
    with source.open("rb") as incoming, rewritten.open("wb") as outgoing:
        for line in incoming:
            match = ARRAY.fullmatch(line)
            if match is None:
                outgoing.write(line)
                continue
            symbol = match[1]
            data.extend(b"\0" * (-len(data) % 16))
            symbols.append((prefix + symbol, len(data)))
            start = len(data)
            for values in incoming:
                if values.rstrip() == b"};":
                    break
                data.extend(bytes.fromhex(values.replace(b"0x", b"").replace(b",", b"").decode("ascii")))
            else:
                raise ValueError("Unterminated wasm2c data segment")
            length = len(data) - start
            outgoing.write(b"extern const u8 " + symbol + b"[" + str(length).encode("ascii") + b"];\n")
    if not symbols:
        raise ValueError("No wasm2c constant byte arrays found")
    return data, symbols


def write_omf(path, data, symbols):
    with path.open("wb") as output:
        record(output, 0x80, name(b"WCDATA"))
        record(output, 0x96, name(b"WCDATA") + name(b"CONST") + b"\0")
        # 32-bit, paragraph aligned, public combine, no overlay.
        record(output, 0x99, struct.pack("<BIBBB", 0x69, len(data), 1, 2, 3))
        for symbol, offset in symbols:
            record(output, 0x91, b"\0\x01" + name(symbol) + struct.pack("<IB", offset, 0))
        for offset in range(0, len(data), 1000):
            record(output, 0xA1, b"\x01" + struct.pack("<I", offset) + data[offset:offset + 1000])
        record(output, 0x8B, b"\0")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("rewritten", type=Path)
    parser.add_argument("object", type=Path)
    parser.add_argument("--stack-abi", action="store_true",
                        help="use undecorated OpenWatcom -3s / GCC symbols")
    args = parser.parse_args()
    data, symbols = collect(args.source, args.rewritten, b"" if args.stack_abi else b"_")
    write_omf(args.object, data, symbols)
    print(f"{len(data):,} data bytes in {len(symbols)} segments")
