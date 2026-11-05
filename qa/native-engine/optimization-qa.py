#!/usr/bin/env python3
"""Time and compare isolated GCC optimization builds with the native QA host.

These are QEMU i386/Linux ABI tests. They do not measure OS/2 PM responsiveness,
resident memory, or page reclamation. Keep both engine build directories intact.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import resource
import statistics
import struct
import subprocess
import time


SUITE = Path(__file__).resolve().parents[2]
FIXTURES = {"word": "SAMPLE.DOCX", "grid": "PILOT.XLSX", "deck": "FIELD.PPTX"}
SCOPE = ("Native GCC i386 OMF engine code with the OpenWatcom Linux CRT, production "
         "reactor/runtime/raster, deterministic test-only RNG and fixed frame times, "
         "executed by QEMU Pentium. The QA host sets the production x87 0x023f mode "
         "at each engine call. This is not OS/2 kernel/PM execution or physical RAM measurement.")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def accessible(path):
    data = path.read_bytes()
    assert data[:4] == b"WCA1" and len(data) >= 16
    at, rows = 16, []
    while at < len(data):
        _, _, flags, _, nl, nd = struct.unpack_from("<6I", data, at)
        at += 24
        assert nl + nd <= len(data) - at
        label = data[at:at + nl].decode("utf-8")
        at += nl
        detail = data[at:at + nd].decode("utf-8")
        at += nd
        rows.append((flags, label, detail))
    assert at == len(data)
    return rows


def check_run(output, app):
    log = (output / "probe.log").read_text()
    assert "PASS native i386 engine init, eight frames" in log
    assert log.count("random_calls=1") == 8
    blank = accessible(output / f"{app}-2.a11y")
    loaded = accessible(output / f"{app}-7.a11y")
    blank_text = "\n".join(label + "\n" + detail for _, label, detail in blank)
    loaded_text = "\n".join(label + "\n" + detail for _, label, detail in loaded)
    if app == "word":
        docs = [detail for flags, _, detail in blank if flags & 16]
        assert docs and all(not text.strip() for text in docs)
        assert "Warp WordCraft" in loaded_text and "Café & résumé" in loaded_text
    elif app == "grid":
        assert "100%" in blank_text and "100%" in loaded_text
        assert "Sales dashboard" not in blank_text and "Quarterly sales" not in blank_text
        assert "NORTHSTAR STUDIO   /   PILOT FORECAST" in loaded_text
    else:
        assert "Northern Lights" not in blank_text
        slides = lambda rows: sum(bool(re.fullmatch(r"Button - Slide \d+", label)) for _, label, _ in rows)
        assert slides(blank) == 1 and slides(loaded) == 3
    files = {p.name: digest(p) for p in sorted(output.iterdir()) if p.suffix in (".a11y", ".wcg", ".ppm")}
    assert len(files) == 18
    return files


def run(args):
    engine, output = args.engine.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    assert not (output / "benchmark.json").exists(), "Use a new output directory to preserve prior evidence"
    command = ["bash", str(SUITE / "qa/native-engine/build-probe.sh"), str(engine), str(output), args.app]
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
    (output / "build.log").write_bytes(result.stdout)
    result.check_returncode()
    qemu = SUITE.parent / "toolchain/usr/bin/qemu-i386-static"
    fixture = SUITE / "examples" / FIXTURES[args.app]
    objects = [engine / "objects" / name for name in ("helpers.obj", "word-data.obj", "word-meta.obj")]
    objects += [engine / "objects" / f"word_{i}.obj" for i in range(1, 128)]
    object_hash = hashlib.sha256()
    for path in objects:
        object_hash.update(path.name.encode() + b"\0" + bytes.fromhex(digest(path)))
    record = {"scope": SCOPE, "app": args.app, "engine": str(engine), "probeSha256": digest(output / "probe"),
              "generatedObjectCount": len(objects), "generatedObjectSetSha256": object_hash.hexdigest(),
              "fixtureSha256": digest(fixture), "runs": []}
    for iteration in range(args.repetitions):
        dest = output / f"run-{iteration + 1}"
        dest.mkdir()
        command = [str(qemu), "-cpu", "pentium", str(output / "probe"), str(dest / args.app), str(fixture)]
        before = resource.getrusage(resource.RUSAGE_CHILDREN)
        start = time.perf_counter()
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
        elapsed = time.perf_counter() - start
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        (dest / "probe.log").write_bytes(result.stdout)
        result.check_returncode()
        files = check_run(dest, args.app)
        if record["runs"]:
            assert files == record["runs"][0]["files"], "Repeated execution produced different output"
        timings = {"wallSeconds": elapsed, "userSeconds": after.ru_utime - before.ru_utime,
                   "systemSeconds": after.ru_stime - before.ru_stime}
        record["runs"].append({"directory": str(dest), **timings, "files": files})
        print(f"PASS {args.app} run {iteration + 1}: wall={elapsed:.3f}s user={timings['userSeconds']:.3f}s", flush=True)
    record["median"] = {key: statistics.median(item[key] for item in record["runs"])
                        for key in ("wallSeconds", "userSeconds", "systemSeconds")}
    record["allRunsByteIdentical"] = True
    (output / "benchmark.json").write_text(json.dumps(record, indent=2) + "\n")


def compare(args):
    baseline = json.loads((args.baseline / "benchmark.json").read_text())
    optimized = json.loads((args.optimized / "benchmark.json").read_text())
    assert baseline["app"] == optimized["app"] and baseline["fixtureSha256"] == optimized["fixtureSha256"]
    left, right = baseline["runs"][0]["files"], optimized["runs"][0]["files"]
    differences = [name for name in sorted(left.keys() | right.keys()) if left.get(name) != right.get(name)]
    report = {"scope": SCOPE, "app": baseline["app"], "baseline": str(args.baseline.resolve()),
              "optimized": str(args.optimized.resolve()), "comparedFiles": len(left),
              "byteIdentical": not differences, "differentFiles": differences,
              "baselineMedian": baseline["median"], "optimizedMedian": optimized["median"],
              "userTimeRatio": optimized["median"]["userSeconds"] / baseline["median"]["userSeconds"],
              "wallTimeRatio": optimized["median"]["wallSeconds"] / baseline["median"]["wallSeconds"],
              "timingCaution": "Three short runs under QEMU on a shared build host; wall time includes output I/O and may be affected by concurrent builds. These figures are not OS/2 performance predictions."}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    assert not differences, "Optimized native output differs; inspect report before shipping"


parser = argparse.ArgumentParser(description=__doc__)
commands = parser.add_subparsers(dest="command", required=True)
runner = commands.add_parser("run")
runner.add_argument("engine", type=Path)
runner.add_argument("output", type=Path)
runner.add_argument("app", choices=FIXTURES)
runner.add_argument("--repetitions", type=int, default=3)
comparer = commands.add_parser("compare")
comparer.add_argument("baseline", type=Path)
comparer.add_argument("optimized", type=Path)
comparer.add_argument("report", type=Path)
arguments = parser.parse_args()
run(arguments) if arguments.command == "run" else compare(arguments)
