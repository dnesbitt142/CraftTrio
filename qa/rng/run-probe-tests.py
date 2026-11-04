#!/usr/bin/env python3
"""Run the exact Watcom i386 RNG code under QEMU Linux user emulation.

This does not boot OS/2 or test Presentation Manager. The native OS/2 console
build uses the same main, RNG service, and jitterentropy platform glue.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--emulator", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    binary, emulator = args.binary.resolve(), args.emulator.resolve()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    results = []

    def run(name, argv=(), cpu="pentium", entropy=False, expected=0):
        with tempfile.TemporaryDirectory(prefix="craft-rng-") as tmp:
            cwd = Path(tmp) / "unrelated-working-directory"
            cwd.mkdir()
            if entropy:
                (cwd / "ENTROPY.BIN").write_bytes(b"x")
            cmd = [str(emulator), "-cpu", cpu, str(binary), *argv]
            start = time.monotonic()
            proc = subprocess.run(cmd, cwd=cwd, capture_output=True,
                                  text=True, timeout=600)
            elapsed = time.monotonic() - start
            record = {"name": name, "cpu": cpu, "arguments": list(argv),
                      "returncode": proc.returncode,
                      "seconds": round(elapsed, 3),
                      "stdout": proc.stdout, "stderr": proc.stderr,
                      "entropy_file_fixture": "one-byte stale file" if entropy else "absent"}
            record["passed"] = proc.returncode == expected
            if expected:
                record["passed"] &= "CPU has no timestamp counter" in proc.stderr
            elif "--kat-only" not in argv:
                record["passed"] &= "PASS all requested tests" in proc.stdout
            if not entropy:
                record["passed"] &= not (cwd / "ENTROPY.BIN").exists()
            return record

    specs = [
        ("known-answer-tests", ("--kat-only",), "pentium", False, 0),
        ("sizes-1-16-32-255-4096", (), "pentium", False, 0),
        ("single-request-over-64k", ("--bytes", "65537"), "pentium", False, 0),
        ("pentium3-no-file", ("--bytes", "32"), "pentium3", False, 0),
        ("stale-one-byte-file-ignored", ("--bytes", "32"), "pentium", True, 0),
        ("486-no-tsc-rejected", ("--bytes", "32"), "486", False, 2),
    ]
    for spec in specs:
        record = run(*spec)
        results.append(record)
        print(f"{record['name']}: {'PASS' if record['passed'] else 'FAIL'} ({record['seconds']:.3f}s)", flush=True)
    with ThreadPoolExecutor(max_workers=2) as pool:
        future = [pool.submit(run, f"concurrent-{i}", ("--bytes", "32")) for i in (1, 2)]
        parallel = [f.result() for f in future]
    pattern = r"PASS read bytes=32 SHA3-256=([a-f0-9]{64})"
    digests = [re.search(pattern, r["stdout"]) for r in parallel]
    distinct = all(digests) and digests[0].group(1) != digests[1].group(1)
    for record in parallel:
        record["passed"] &= bool(distinct)
        record["distinct_process_output"] = bool(distinct)
        results.append(record)
        print(f"{record['name']}: {'PASS' if record['passed'] else 'FAIL'} ({record['seconds']:.3f}s)", flush=True)
    report = {"scope": "Watcom i386 machine code under QEMU Linux user emulation; no OS/2 guest test",
              "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
              "emulator_sha256": hashlib.sha256(emulator.read_bytes()).hexdigest(),
              "results": results,
              "passed": all(r["passed"] for r in results)}
    (out / "watcom-probe-tests.json").write_text(json.dumps(report, indent=2) + "\n")
    with (out / "watcom-probe-tests.log").open("w") as log:
        log.write(report["scope"] + "\n")
        for record in results:
            log.write(f"\n=== {record['name']} cpu={record['cpu']} rc={record['returncode']} seconds={record['seconds']} ===\n")
            log.write(record["stdout"])
            log.write(record["stderr"])
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
