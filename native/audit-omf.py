#!/usr/bin/env python3
"""Independently compare ELF32 relocation source offsets/modes with OMF FIXUPP.

This is deliberately limited to the i386 REL input and 32-bit, explicit FIXUPP
output used by build-engine.sh. Unknown relocation encodings fail closed. It
checks multiplicity, source offset and direct/self-relative mode, plus record
bounds/checksums; it does not prove target-symbol or addend equivalence.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct
import sys


class AuditError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise AuditError(message)


def span(data, offset, size, label):
    require(0 <= offset <= len(data) and 0 <= size <= len(data) - offset,
            f"{label}: range {offset}+{size} exceeds {len(data)} bytes")
    return data[offset:offset + size]


def c_string(data, offset, label):
    require(0 <= offset < len(data), f"{label}: invalid string offset")
    end = data.find(b"\0", offset)
    require(end >= 0, f"{label}: unterminated string")
    return data[offset:end].decode("ascii", "strict")


def elf_relocations(data):
    require(len(data) >= 52 and data[:7] == b"\x7fELF\x01\x01\x01",
            "expected little-endian ELF32")
    header = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    etype, machine, version, _, _, shoff, _, ehsize, _, _, shsize, shnum, shstr = header
    require((etype, machine, version) == (1, 3, 1), "expected relocatable i386 ELF")
    require(ehsize == 52 and shsize == 40 and shnum > 0 and 0 < shstr < shnum,
            "unsupported ELF section table")
    span(data, shoff, shnum * 40, "ELF section table")
    sections = [struct.unpack_from("<10I", data, shoff + n * 40) for n in range(shnum)]
    strings = sections[shstr]
    require(strings[1] == 3, "ELF section-name table is not STRTAB")
    names = span(data, strings[4], strings[5], "ELF section names")
    section_names = [c_string(names, section[0], "ELF section name") for section in sections]
    # Same-named COFF sections share one OMF segment, in source order, with
    # each contribution aligned. Generated compiler objects normally are unique.
    merged_sizes = {}
    bases = {}
    for index, section in enumerate(sections):
        _, kind, flags, _, offset, size, _, _, alignment, _ = section
        if kind not in (1, 8):
            continue
        name = section_names[index]
        require(len(name.encode("ascii")) <= 255, "OMF segment-name truncation is unsupported")
        require(alignment == 0 or alignment & (alignment - 1) == 0,
                "ELF section alignment is not a power of two")
        require(alignment <= 8192, "ELF alignment exceeds COFF conversion limit")
        alignment = max(1, alignment)
        base = (merged_sizes.get(name, 0) + alignment - 1) & -alignment
        bases[index] = base
        merged_sizes[name] = base + size
        if kind != 8:
            span(data, offset, size, f"ELF {name} contents")
    result = Counter()
    for section in sections:
        _, kind, _, _, offset, size, link, target, _, entrysize = section
        if kind not in (4, 9):
            continue
        require(0 < target < shnum and 0 < link < shnum,
                "ELF relocation section has invalid links")
        target_section = sections[target]
        if not target_section[2] & 2:  # SHF_ALLOC
            continue
        require(kind == 9 and entrysize == 8 and size % 8 == 0,
                "only ELF32 SHT_REL relocation entries are supported")
        require(target in bases, "relocation target is not program data")
        symtab = sections[link]
        require(symtab[1] == 2 and symtab[9] == 16 and symtab[5] % 16 == 0,
                "invalid ELF relocation symbol table")
        span(data, symtab[4], symtab[5], "ELF symbol table")
        rows = span(data, offset, size, "ELF relocation table")
        for source, info in struct.iter_unpack("<II", rows):
            mode = info & 255
            if mode == 0:  # R_386_NONE has no fixup
                continue
            require(mode in (1, 2), f"unsupported i386 relocation {mode}")
            require(info >> 8 < symtab[5] // 16, "ELF relocation symbol index out of range")
            require(source <= target_section[5] and target_section[5] - source >= 4,
                    "ELF relocation source exceeds its section")
            result[(section_names[target], bases[target] + source, int(mode == 1))] += 1
    return result


class Cursor:
    def __init__(self, data, label):
        self.data, self.pos, self.label = data, 0, label

    def take(self, count):
        value = span(self.data, self.pos, count, self.label)
        self.pos += count
        return value

    def number(self, count):
        return int.from_bytes(self.take(count), "little")

    def index(self):
        first = self.number(1)
        return ((first & 127) << 8) | self.number(1) if first & 128 else first

    def done(self):
        return self.pos == len(self.data)


def omf_records(data):
    offset = 0
    count = 0
    ended = False
    while offset < len(data):
        require(not ended, "data follows OMF MODEND")
        header = span(data, offset, 3, "OMF record header")
        kind, length = struct.unpack("<BH", header)
        require(length >= 1, "OMF record has no checksum")
        full = span(data, offset, length + 3, "OMF record")
        require(sum(full) & 255 == 0, f"OMF checksum mismatch at {offset}")
        require(count > 0 or kind == 0x80, "OMF does not begin with THEADR")
        yield kind, full[3:-1], offset
        count += 1
        offset += length + 3
        ended = kind in (0x8a, 0x8b)
    require(count > 0 and ended, "OMF is missing MODEND")


def omf_relocations(data):
    names = [None]
    segments = [None]
    last_data = None
    result = Counter()
    for kind, payload, offset in omf_records(data):
        cursor = Cursor(payload, f"OMF record at {offset}")
        if kind == 0x96:  # LNAMES
            while not cursor.done():
                names.append(cursor.take(cursor.number(1)).decode("ascii", "strict"))
        elif kind in (0x98, 0x99):  # SEGDEF
            attributes = cursor.number(1)
            require(attributes >> 5 != 0, "absolute OMF segments are unsupported")
            size = cursor.number(4 if kind & 1 else 2)
            if attributes & 2:
                size = 1 << (32 if kind & 1 else 16)
            name, class_name, overlay = cursor.index(), cursor.index(), cursor.index()
            require(0 < name < len(names) and 0 < class_name < len(names)
                    and overlay < len(names), "invalid OMF segment name index")
            require(cursor.done(), "trailing OMF SEGDEF data")
            segments.append((names[name], size))
        elif kind in (0xa0, 0xa1):  # LEDATA
            segment = cursor.index()
            require(0 < segment < len(segments), "invalid OMF LEDATA segment")
            base = cursor.number(4 if kind & 1 else 2)
            length = len(payload) - cursor.pos
            require(base <= segments[segment][1] and length <= segments[segment][1] - base,
                    "OMF LEDATA exceeds its segment")
            last_data = (segment, base, length)
        elif kind in (0xa2, 0xa3):
            raise AuditError("LIDATA is unsupported by the relocation audit")
        elif kind in (0x9c, 0x9d):  # FIXUPP
            require(last_data is not None, "OMF FIXUPP has no preceding LEDATA")
            segment, base, length = last_data
            while not cursor.done():
                locat = cursor.number(1) << 8 | cursor.number(1)
                require(locat & 0x8000, "OMF THREAD subrecords are unsupported")
                require((locat >> 10) & 15 == 9, "expected a 32-bit OMF offset fixup")
                fixdat = cursor.number(1)
                require(not fixdat & 0x88, "OMF threaded frame/target is unsupported")
                frame_method = (fixdat >> 4) & 7
                require(frame_method <= 5, "invalid OMF frame method")
                if frame_method < 4:
                    require(cursor.index() > 0, "invalid OMF frame datum")
                require(cursor.index() > 0, "invalid OMF target datum")
                if not fixdat & 4:
                    cursor.number(4 if kind & 1 else 2)
                source = locat & 1023
                require(source <= length and length - source >= 4,
                        "OMF fixup exceeds its LEDATA record")
                result[(segments[segment][0], base + source, (locat >> 14) & 1)] += 1
        elif kind in (0x8a, 0x8b):
            last_data = None
    return result


def examples(counter):
    return [{"section": name, "offset": offset, "mode": "direct" if mode else "self-relative",
             "count": count} for (name, offset, mode), count in sorted(counter.items())[:10]]


def audit(elf_path, omf_path):
    elf_data, omf_data = Path(elf_path).read_bytes(), Path(omf_path).read_bytes()
    report = {"elf": str(elf_path), "omf": str(omf_path),
              "elf_sha256": hashlib.sha256(elf_data).hexdigest(),
              "omf_sha256": hashlib.sha256(omf_data).hexdigest(),
              "scope": "relocation multiplicity, source section/offset and direct/self-relative mode; record bounds/checksums"}
    try:
        expected, actual = elf_relocations(elf_data), omf_relocations(omf_data)
        missing, extra = expected - actual, actual - expected
        report.update(expected=sum(expected.values()), actual=sum(actual.values()),
                      missing=sum(missing.values()), extra=sum(extra.values()),
                      missing_examples=examples(missing), extra_examples=examples(extra),
                      passed=not missing and not extra)
    except (AuditError, UnicodeError, struct.error) as error:
        report.update(passed=False, error=str(error))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("omf", type=Path)
    parser.add_argument("--output", "--report", dest="output", type=Path)
    args = parser.parse_args()
    try:
        report = audit(args.elf, args.omf)
    except OSError as error:
        report = {"passed": False, "error": str(error)}
    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded)
    print(json.dumps(report, sort_keys=True))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
