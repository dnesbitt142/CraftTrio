#!/usr/bin/env python3
"""Qualify objconv's large i386 ELF/COFF -> OMF relocation conversion.

No application toolchain or VM is required. Fixtures cross the 16-bit COFF
relocation-count boundary and a late relocated section after COFF table growth,
exercise ABS/NONE advancement and mixed modes, and reject malformed sentinels.
An independent parser checks every source section/offset/mode; checksum-preserving
corruptions must also be detected.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys


class TestFailure(RuntimeError):
    pass


def check(condition, message):
    if not condition:
        raise TestFailure(message)


def make_elf(path, types):
    """Construct a minimal ELF32 object, independent of assembler availability."""
    text = bytes(len(types) * 4)
    relocations = b"".join(struct.pack("<II", index * 4, (1 << 8) | kind)
                           for index, kind in enumerate(types))
    symbols = bytes(16) + struct.pack("<IIIBBH", 1, 0, 0, 0x10, 0, 0)
    strings = b"\0external_target\0"
    names = b"\0.text\0.rel.text\0.symtab\0.strtab\0.shstrtab\0"
    blobs = [b"", text, relocations, symbols, strings, names]
    # name, type, flags, alignment, entry size
    attributes = [(0, 0, 0, 0, 0), (1, 1, 6, 4, 0), (7, 9, 0, 4, 8),
                  (17, 2, 0, 4, 16), (25, 3, 0, 1, 0), (33, 3, 0, 1, 0)]
    data = bytearray(52)
    sections = [(0,) * 10]
    for index, blob in enumerate(blobs[1:], 1):
        name, kind, flags, alignment, entrysize = attributes[index]
        data.extend(bytes(-len(data) % alignment))
        offset = len(data)
        data.extend(blob)
        sections.append((name, kind, flags, 0, offset, len(blob),
                         3 if index == 2 else 4 if index == 3 else 0,
                         1 if index in (2, 3) else 0, alignment, entrysize))
    data.extend(bytes(-len(data) % 4))
    section_offset = len(data)
    for section in sections:
        data.extend(struct.pack("<10I", *section))
    data[:16] = b"\x7fELF\x01\x01\x01" + bytes(9)
    struct.pack_into("<HHIIIIIHHHHHH", data, 16, 1, 3, 1, 0, 0, section_offset,
                     0, 52, 0, 0, 40, 6, 5)
    path.write_bytes(data)


def make_late_relocation_elf(path, count=1024):
    """Exercise the base ELF bounds fix independently of count overflow.

    COFF relocation entries are larger than ELF REL entries. A relocated data
    section after a heavily relocated code section therefore has its output
    offset beyond the entire original ELF file, while remaining valid within
    its own source section. Comparing those two address spaces rejects it.
    """
    names = b"\0.text\0.rel.text\0.data\0.rel.data\0.symtab\0.strtab\0.shstrtab\0"
    relocations = b"".join(struct.pack("<II", index * 4, 0x101)
                           for index in range(count))
    symbols = bytes(16) + struct.pack("<IIIBBH", 1, 0, 0, 0x10, 0, 0)
    # name, type, flags, alignment, entry size, bytes, link, info
    specs = [
        (b".text", 1, 6, 4, 0, bytes(count * 4), 0, 0),
        (b".rel.text", 9, 0, 4, 8, relocations, 5, 1),
        (b".data", 1, 3, 4, 0, bytes(4), 0, 0),
        (b".rel.data", 9, 0, 4, 8, struct.pack("<II", 0, 0x101), 5, 3),
        (b".symtab", 2, 0, 4, 16, symbols, 6, 1),
        (b".strtab", 3, 0, 1, 0, b"\0external_target\0", 0, 0),
        (b".shstrtab", 3, 0, 1, 0, names, 0, 0),
    ]
    data = bytearray(52)
    sections = [(0,) * 10]
    for name, kind, flags, alignment, entrysize, blob, link, info in specs:
        data.extend(bytes(-len(data) % alignment))
        offset = len(data)
        data.extend(blob)
        sections.append((names.index(name + b"\0"), kind, flags, 0, offset,
                         len(blob), link, info, alignment, entrysize))
    data.extend(bytes(-len(data) % 4))
    section_offset = len(data)
    for section in sections:
        data.extend(struct.pack("<10I", *section))
    data[:16] = b"\x7fELF\x01\x01\x01" + bytes(9)
    struct.pack_into("<HHIIIIIHHHHHH", data, 16, 1, 3, 1, 0, 0, section_offset,
                     0, 52, 0, 0, 40, len(sections), len(sections) - 1)
    path.write_bytes(data)


def load_audit():
    path = Path(__file__).resolve().parents[2] / "native" / "audit-omf.py"
    spec = importlib.util.spec_from_file_location("craft_omf_audit", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def convert(converter, kind, source, destination, expect_success=True):
    destination.unlink(missing_ok=True)
    try:
        process = subprocess.run([str(converter), kind, "-v0", str(source), str(destination)],
                                 capture_output=True, text=True, timeout=45)
    except subprocess.TimeoutExpired as error:
        raise TestFailure(f"conversion timed out: {source.name}") from error
    log = process.stdout + process.stderr
    if expect_success:
        check(process.returncode == 0 and destination.is_file(),
              f"conversion failed for {source.name}: status {process.returncode}: {log[-1000:]}")
    else:
        # A signal/crash is not a valid rejection of a malformed object.
        check(process.returncode > 0,
              f"malformed {source.name} was accepted or crashed: {process.returncode}: {log[-500:]}")
    return {"returncode": process.returncode, "diagnostic": log[-1000:]}


def coff_header(data):
    check(len(data) >= 60 and struct.unpack_from("<HH", data) == (0x14c, 1),
          "fixture conversion did not produce one-section i386 COFF")
    header = struct.unpack_from("<8sIIIIIIHHI", data, 20)
    return {"relocations_offset": header[5], "count": header[7], "flags": header[9]}


def record_audit(auditor, source, destination, report_path, expect_success=True):
    report = auditor.audit(source, destination)
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    check(report["passed"] == expect_success,
          f"unexpected relocation audit result for {destination.name}: {json.dumps(report)}")
    return report


def change_fixup(auditor, data, change):
    result = bytearray(data)
    for kind, payload, offset in auditor.omf_records(data):
        if kind in (0x9c, 0x9d) and payload:
            if change == "mode":
                result[offset + 3] ^= 0x40
            elif change == "offset":
                # First fixture fixup is at zero; changing its low byte to one
                # stays inside LEDATA but shifts the relocation's source.
                result[offset + 4] ^= 1
            else:
                raise AssertionError(change)
            length = len(payload) + 1
            result[offset + 3 + len(payload)] = (-sum(result[offset:offset + length + 2])) & 255
            return result
    raise TestFailure("fixture contains no FIXUPP record")


def run(converter, output):
    auditor = load_audit()
    results = {"converter": str(converter),
               "converter_sha256": hashlib.sha256(converter.read_bytes()).hexdigest(),
               "passed": False, "boundary": [], "malformed": [], "audit_rejections": []}
    source = output / "later-section.o"
    make_late_relocation_elf(source)
    coff = output / "later-section.coff"
    results["multi_section_bounds"] = []
    for name, input_path in (("direct", source), ("staged", coff)):
        if name == "staged":
            convert(converter, "-fcoff32", source, coff)
        destination = output / ("later-section-" + name + ".obj")
        convert(converter, "-fomf32", input_path, destination)
        report = record_audit(auditor, source, destination,
                              output / ("later-section-" + name + ".json"))
        check(report["expected"] == 1025, "incorrect multi-section fixture count")
        results["multi_section_bounds"].append({"path": name,
                                               "relocations": report["actual"]})
    for count in (65535, 65536, 82009):
        source = output / f"reloc-{count}.o"
        coff = output / f"reloc-{count}.coff"
        direct = output / f"reloc-{count}-direct.obj"
        staged = output / f"reloc-{count}-staged.obj"
        make_elf(source, [1] * count)
        convert(converter, "-fomf32", source, direct)
        direct_report = record_audit(auditor, source, direct,
                                     output / f"reloc-{count}-direct.json")
        check(direct_report["expected"] == count, "incorrect fixture count")
        convert(converter, "-fcoff32", source, coff)
        data = coff.read_bytes()
        header = coff_header(data)
        check(header["count"] == min(count, 65535), "incorrect COFF section relocation count")
        check(bool(header["flags"] & 0x01000000) == (count > 65535),
              "incorrect COFF relocation overflow flag")
        if count > 65535:
            check(struct.unpack_from("<IIH", data, header["relocations_offset"]) == (count + 1, 0, 0),
                  "incorrect COFF extended count sentinel")
        convert(converter, "-fomf32", coff, staged)
        staged_report = record_audit(auditor, source, staged,
                                     output / f"reloc-{count}-staged.json")
        results["boundary"].append({"count": count, "direct": direct_report["actual"],
                                    "staged": staged_report["actual"], "coff": header})
    # ABS first with an intentionally unusable symbol index proves it is skipped
    # before symbol lookup and that the reader advances to following relocations.
    source = output / "mixed.o"
    make_elf(source, [0, 1, 2, 0, 1])
    coff = output / "mixed.coff"
    convert(converter, "-fcoff32", source, coff)
    mixed = bytearray(coff.read_bytes())
    first = coff_header(mixed)["relocations_offset"]
    struct.pack_into("<I", mixed, first + 4, 0xffffffff)
    coff.write_bytes(mixed)
    for name, input_path in (("direct", source), ("staged", coff)):
        destination = output / f"mixed-{name}.obj"
        convert(converter, "-fomf32", input_path, destination)
        report = record_audit(auditor, source, destination, output / f"mixed-{name}.json")
        check(report["actual"] == 3, "ABS/NONE advancement lost a relocation")
    results["mixed_mode_and_abs"] = True
    # Also accept LLVM's valid boundary encoding: 65,535 real relocations plus
    # one synthetic count entry. The writer may use ordinary 0xffff instead.
    small = bytearray((output / "reloc-65535.coff").read_bytes())
    header = coff_header(small)
    pointer = header["relocations_offset"]
    symbol_pointer = struct.unpack_from("<I", small, 8)[0]
    small[pointer:pointer] = struct.pack("<IIH", 65536, 0, 0)
    struct.pack_into("<I", small, 8, symbol_pointer + 10)
    struct.pack_into("<I", small, 56, header["flags"] | 0x01000000)
    alternate = output / "reloc-65535-extended.coff"
    alternate.write_bytes(small)
    destination = output / "reloc-65535-extended.obj"
    convert(converter, "-fomf32", alternate, destination)
    record_audit(auditor, output / "reloc-65535.o", destination,
                 output / "reloc-65535-extended.json")
    results["extended_65535_accepted"] = True
    original = (output / "reloc-82009.coff").read_bytes()
    header = coff_header(original)
    pointer = header["relocations_offset"]
    mutations = {
        "header-count": (52, "<H", 65534),
        "truncated-sentinel": (44, "<I", len(original) - 5),
        "small-count": (pointer, "<I", 65535),
        "sentinel-symbol": (pointer + 4, "<I", 1),
        "sentinel-type": (pointer + 8, "<H", 6),
        "count-past-end": (pointer, "<I", 0xffffffff),
        "zero-pointer": (44, "<I", 0),
        "pointer-past-end": (44, "<I", 0xfffffffc),
    }
    for name, (offset, encoding, value) in mutations.items():
        malformed = bytearray(original)
        struct.pack_into(encoding, malformed, offset, value)
        path = output / f"bad-{name}.coff"
        path.write_bytes(malformed)
        result = convert(converter, "-fomf32", path, output / f"bad-{name}.obj", False)
        results["malformed"].append({"case": name, **result})
    # A formally ordinary table with a cleared flag is ambiguous to the converter;
    # the independent source comparison must catch its silently shortened output.
    noflag = bytearray(original)
    struct.pack_into("<I", noflag, 56, header["flags"] & ~0x01000000)
    path = output / "bad-cleared-flag.coff"
    path.write_bytes(noflag)
    destination = output / "bad-cleared-flag.obj"
    convert(converter, "-fomf32", path, destination)
    record_audit(auditor, output / "reloc-82009.o", destination,
                 output / "bad-cleared-flag.json", False)
    results["audit_rejections"].append("cleared-overflow-flag")
    reference = (output / "reloc-65535-direct.obj").read_bytes()
    for change in ("mode", "offset"):
        destination = output / f"bad-fixup-{change}.obj"
        destination.write_bytes(change_fixup(auditor, reference, change))
        report = record_audit(auditor, output / "reloc-65535.o", destination,
                              output / f"bad-fixup-{change}.json", False)
        check(report.get("actual") == report.get("expected") == 65535,
              "corruption fixture must preserve total count")
        results["audit_rejections"].append(f"checksum-preserving-{change}")
    for name, data in (("checksum", reference[:10] + bytes([reference[10] ^ 1]) + reference[11:]),
                       ("truncated", reference[:-1])):
        destination = output / f"bad-{name}.obj"
        destination.write_bytes(data)
        record_audit(auditor, output / "reloc-65535.o", destination,
                     output / f"bad-{name}.json", False)
        results["audit_rejections"].append(name)
    results["passed"] = True
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("converter", type=Path)
    parser.add_argument("--output", type=Path, default=Path("build/qa/objconv"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    try:
        report = run(args.converter.resolve(), args.output.resolve())
    except (OSError, ValueError, struct.error, TestFailure) as error:
        report = {"passed": False, "error": str(error)}
    (args.output / "summary.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps(report, sort_keys=True))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
