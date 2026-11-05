#!/usr/bin/env python3
"""Test fresh pages in the actual OS/2 runtime using protected Linux shims.

mincore measures the shim's residency before byte checks. It is not OS/2 RSS.
Run from the kit root. Unchanged WABT support defaults to native/runtime; an
overlay-only checkout may supply --support-dir pointing at WABT 1.0.34 wasm2c.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import zipfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    suite = Path(__file__).resolve().parents[2]
    qa, runtime = suite / "qa/paging", suite / "native/runtime"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=suite / "build/paging")
    parser.add_argument("--support-dir", type=Path, default=runtime)
    baseline = parser.add_mutually_exclusive_group()
    baseline.add_argument("--baseline-runtime", type=Path)
    baseline.add_argument("--baseline-archive", type=Path,
                          help="source ZIP containing native/runtime/wasm-rt-impl.c")
    args = parser.parse_args()
    output, support = args.output.resolve(), args.support_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    for name in ("wasm-rt-impl.h", "wasm-rt-exceptions.h", "wasm-rt-exceptions-impl.c"):
        if not (support / name).is_file():
            parser.error("Missing unchanged WABT 1.0.34 support: " + str(support / name))
    inherited = suite / "qa/reclaim/runtime-pages.c"
    base = inherited.read_text()
    # Expand the existing shim to four MiB. Dirty partial recommits still model
    # cache recovery; failed fresh growth cannot be dirtied by valid callers.
    replacements = {
        "unsigned char committed[512]": "unsigned char committed[1024]",
        "memset(address,0x9a,4096);": "if (!qa_fresh_growth) memset(address,0x9a,4096);",
    }
    for old, new in replacements.items():
        if base.count(old) != 1:
            parser.error("Review changed reclaim shim before extending it: " + old)
        base = base.replace(old, new)
    (output / "protected-base.inc").write_text(base)
    baseline_source = args.baseline_runtime.resolve() if args.baseline_runtime else None
    if args.baseline_archive:
        baseline_source = output / "baseline-runtime.c"
        with zipfile.ZipFile(args.baseline_archive) as archive:
            baseline_source.write_bytes(archive.read("native/runtime/wasm-rt-impl.c"))
    report = {
        "scope": "Linux protected OS/2 API shims; mincore before byte checks; not OS/2 RSS",
        "productionRuntimeSha256": digest(runtime / "wasm-rt-impl.c"),
        "inheritedReclaimTestSha256": digest(inherited),
        "supportHashes": {name: digest(support / name) for name in (
            "wasm-rt-impl.h", "wasm-rt-exceptions.h", "wasm-rt-exceptions-impl.c")},
        "runs": [],
    }
    common = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c99", "-g", "-D__OS2__", "-DWASM_RT_USE_MMAP=0",
        "-DWASM_RT_SKIP_SIGNAL_RECOVERY=1", "-I" + str(suite / "qa/reclaim/os2-stub"),
        "-I" + str(runtime), "-I" + str(support), "-I" + str(output), str(qa / "host-fresh.c")]
    cases = [("after", runtime / "wasm-rt-impl.c", False, False),
             ("after-sanitized", runtime / "wasm-rt-impl.c", False, True)]
    if baseline_source:
        cases.insert(0, ("before", baseline_source, True, False))
        report["baselineRuntimeSha256"] = digest(baseline_source)
    for name, source, eager, sanitize in cases:
        executable = output / ("fresh-" + name)
        command = list(common)
        if sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
        command += [str(source), str(support / "wasm-rt-exceptions-impl.c"),
                    "-lm", "-o", str(executable)]
        subprocess.run(command, check=True, timeout=90)
        process = subprocess.run([str(executable)] + (["eager"] if eager else []),
                                 cwd=output, capture_output=True, text=True, timeout=30)
        text = process.stdout + process.stderr
        (output / (name + ".log")).write_text(text)
        expected = "256" if eager else "0"
        passed = process.returncode == 0 and all(mark in process.stdout for mark in (
            "PASS fresh demand-zero initial_resident_pages=" + expected,
            "grown_resident_pages=" + expected, "PASS fresh zero contents",
            "PASS checked ownership", "PASS ledger exhaustion")) \
            and "runtime error:" not in text and "ERROR: AddressSanitizer" not in text
        report["runs"].append({"name": name, "passed": passed,
            "returncode": process.returncode, "stdout": process.stdout,
            "stderr": process.stderr})
        print(("PASS " if passed else "FAIL ") + name)
        if not passed:
            print(text)
    report["passed"] = all(run["passed"] for run in report["runs"])
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print("Report: " + str(output / "report.json"))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
