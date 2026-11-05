#!/usr/bin/env python3
"""Generate independent integer/IEEE expectations; no production code imported."""
import math
from pathlib import Path
import random
import struct
import sys

MASK = (1 << 64) - 1
tests = []


def add(name, a, b, expected=0, mode=0):
    tests.append((name, a & MASK, b & MASK, expected & MASK, mode))


def signed(x):
    return x if x < 1 << 63 else x - (1 << 64)


def bits(x, width=64):
    return int.from_bytes(struct.pack("<d" if width == 64 else "<f", x), "little")


def number(x, width=64):
    return struct.unpack("<d" if width == 64 else "<f", x.to_bytes(width // 8, "little"))[0]


edge = [0, 1, 2, 3, (1 << 31) - 1, 1 << 31, (1 << 32) - 1, 1 << 32,
        (1 << 63) - 1, 1 << 63, MASK - 1, MASK]
pairs = [(a, b) for a in edge for b in edge if b]
rng = random.Random(0x4352414654)
pairs += [(rng.getrandbits(64), rng.getrandbits(64) or 1) for _ in range(100)]
for a, b in pairs:
    add("udiv", a, b, a // b)
    add("urem", a, b, a % b)
    sa, sb = signed(a), signed(b)
    if sa == -(1 << 63) and sb == -1:
        add("sdiv", a, b, mode=5)  # WASM_RT_TRAP_INT_OVERFLOW + 3
    else:
        quotient = abs(sa) // abs(sb)
        if (sa < 0) != (sb < 0):
            quotient = -quotient
        add("sdiv", a, b, quotient)
    remainder = abs(sa) % abs(sb)
    add("srem", a, b, -remainder if sa < 0 else remainder)
for a in edge + [rng.getrandbits(64) for _ in range(50)]:
    add("clz", a, 0, 64 - a.bit_length())
    add("ctz", a, 0, 64 if not a else (a & -a).bit_length() - 1)
    add("popcnt", a, 0, a.bit_count())

for width in (64, 32):
    values = [0.0, -0.0, 0.1, -0.1, 0.5, -0.5, 1.5, -1.5, 2.5, -2.5, 3.5, -3.5,
              1.999999, -1.999999, 4503599627370495.5 if width == 64 else 8388607.5,
              float("inf"), -float("inf")]
    for value in values:
        source = bits(value, width)
        value = number(source, width)
        for op in ("nearest", "ceil", "floor", "trunc"):
            if not math.isfinite(value) or value == 0:
                expected = value
            else:
                expected = {"nearest": round, "ceil": math.ceil, "floor": math.floor, "trunc": math.trunc}[op](value)
                if expected == 0:
                    expected = math.copysign(0.0, value)
            add(f"{op}{width}", source, 0, bits(float(expected), width))
    nan_patterns = ([0x7ff8000000000001, 0xfff8000000000001, 0x7ff0000000000001, 0xfff0000000000001]
                    if width == 64 else [0x7fc00001, 0xffc00001, 0x7f800001, 0xff800001])
    for source in nan_patterns:
        for op in ("nearest", "ceil", "floor", "trunc"):
            add(f"{op}{width}", source, 0, mode=1 if width == 64 else 2)

for op, a, b, expected in [
    ("add64", 1.5, 2.25, 3.75), ("add64", 2.0**53, 1.0, 2.0**53),
    ("sub64", 1.0, 0.25, 0.75), ("mul64", 1.5, 0.5, 0.75),
    ("div64", 1.0, 10.0, 0.1), ("mul64", -0.0, 2.0, -0.0),
    ("add64", -0.0, -0.0, -0.0), ("div64", -0.0, 2.0, -0.0),
    ("min64", -0.0, 0.0, -0.0), ("min64", 0.0, -0.0, -0.0),
    ("max64", -0.0, 0.0, 0.0), ("max64", 0.0, -0.0, 0.0),
    ("copy64", 1.5, -0.0, -1.5), ("copy64", -1.5, 0.0, 1.5),
    ("sqrt64", 4.0, 0.0, 2.0), ("sqrt64", -0.0, 0.0, -0.0)]:
    add(op, bits(a), bits(b), bits(expected))
for op, a, b, expected in [
    ("add32", 1.5, 2.25, 3.75), ("add32", 2.0**24, 1.0, 2.0**24),
    ("add32", -0.0, -0.0, -0.0), ("mul32", 1.5, 0.5, 0.75),
    ("mul32", -0.0, 2.0, -0.0)]:
    add(op, bits(a, 32), bits(b, 32), bits(expected, 32))
for op in ("add64", "sub64", "mul64", "div64", "min64", "max64"):
    add(op, 0x7ff0000000000001, bits(1.0), mode=1)
    add(op, bits(1.0), 0xfff8000000000001, mode=1)
add("sqrt64", bits(-1.0), 0, mode=1)
for value in [0.0, -0.0, 1.5, -1.5, 1.0 / 3.0, 16777217.0, float("inf"), -float("inf")]:
    add("demote", bits(value), 0, bits(value, 32))
    add("promote", bits(value, 32), 0, bits(number(bits(value, 32), 32)))
add("demote", 0x7ff0000000000001, 0, mode=2)
add("promote", 0x7f800001, 0, mode=1)
for op in ("udiv", "urem", "sdiv", "srem"):
    add(op, 123, 0, mode=6)  # DIV_BY_ZERO + 3
add("sdiv", 1 << 63, MASK, mode=5)
add("toint", 0x7ff8000000000001, 0, mode=7)
add("toint", bits(float(1 << 63)), 0, mode=5)
add("toint", bits(-float(1 << 63)), 0, 1 << 63)
add("toint", bits(12.75), 0, 12)
add("toint", bits(-12.75), 0, -12)
add("load", 65528, 0, 0)
add("load", 65529, 0, mode=4)  # OOB + 3
add("load", MASK, 0, mode=4)
add("recurse", 10, 0, 0)
add("recurse", 100000, 0, mode=12)  # EXHAUSTION + 3

lines = ["static const Test tests[]={"]
for name, a, b, expected, mode in tests:
    lines.append(f'{{"{name}", w2c_numeric_{name}, UINT64_C(0x{a:016x}), UINT64_C(0x{b:016x}), UINT64_C(0x{expected:016x}), {mode}}},')
lines.append("};")
Path(sys.argv[1]).write_text("\n".join(lines) + "\n")
print(f"Generated {len(tests)} independent expected cases")
