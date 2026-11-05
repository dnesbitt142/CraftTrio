#!/usr/bin/env python3
"""Verify actual direct-input source, including exact Watcom i386 code.

No OS/2 APIs are used in input.c. These tests do not exercise the file chooser,
OS/2 loader, Presentation Manager, or final application's file-accept callback.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    kit = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=kit.parent / "build/memory-input")
    parser.add_argument("--watcom", type=Path,
                        default=Path(os.environ.get("WATCOM", str(kit.parent / "toolchain/openwatcom"))))
    parser.add_argument("--qemu", type=Path,
                        default=kit.parent / "toolchain/usr/bin/qemu-i386-static")
    args = parser.parse_args()
    build = args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    source = kit / "qa/memory/test-input.c"
    log = []
    records = []

    def run(label, command, **kwargs):
        result = subprocess.run([str(x) for x in command], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=180, **kwargs)
        log.append("$ " + " ".join(str(x) for x in command))
        log.append(result.stdout)
        print(result.stdout, end="", flush=True)
        records.append({"label": label, "returncode": result.returncode,
                        "passed": result.returncode == 0})
        if result.returncode:
            raise RuntimeError(f"{label} failed: exit {result.returncode}")

    try:
        cc = os.environ.get("CC", "gcc")
        native = build / "test-input-host"
        run("GCC build", [cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-O2",
                          source, "-o", native])
        fixture = build / "fixture directory with spaces"
        fixture.mkdir(exist_ok=True)
        run("GCC runtime", [native, fixture])

        sanitized = build / "test-input-sanitized"
        run("ASan/UBSan build", [cc, "-std=c99", "-Wall", "-Wextra", "-O1", "-g",
                                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                                source, "-o", sanitized])
        run("ASan/UBSan runtime", [sanitized, fixture],
            env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})

        watcom = args.watcom.resolve()
        if not (watcom / "binl64/wcc386").is_file():
            raise RuntimeError("OpenWatcom compiler not available; supply --watcom")
        qemu = args.qemu.resolve()
        if not qemu.is_file():
            raise RuntimeError("qemu-i386 not available; supply --qemu")
        environment = {**os.environ, "WATCOM": str(watcom),
                       "INCLUDE": str(watcom / "h"),
                       "PATH": str(watcom / "binl64") + os.pathsep + os.environ["PATH"]}
        obj = build / "test-input.obj"
        run("Watcom i386 build", [watcom / "binl64/wcc386", "-q", "-bt=linux", "-3s",
                                   "-zp4", "-mf", "-zastd=c99", "-ot",
                                   "-d_SSIZE_T_DEFINED", "-d__OBSCURE_STREAM_INTERNALS",
                                   "-fo=" + str(obj), source], env=environment)
        binary = build / "test-input-watcom"
        link = build / "input.lnk"
        link.write_text("system linux\noption quiet\noption stack=1048576\n"
                        + "name " + str(binary) + "\nfile " + str(obj) + "\n")
        run("Watcom i386 link", [watcom / "binl64/wlink", "@" + str(link)], env=environment)
        for cpu in ["pentium", "pentium3"]:
            run("Watcom runtime " + cpu, [qemu, "-cpu", cpu, binary, fixture])
        report = {"scope": "Actual input.c: GCC and Watcom Linux i386; no OS/2 guest",
                  "input_sha256": hashlib.sha256((kit / "native/host/input.c").read_bytes()).hexdigest(),
                  "watcom_binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                  "checks": records, "passed": True,
                  "instrumentation": "ASan/UBSan enabled; LeakSanitizer disabled because host lacks /proc; explicit handle/allocation counters cover module resources"}
        (build / "input-results.json").write_text(json.dumps(report, indent=2) + "\n")
    finally:
        (build / "input-tests.log").write_text("\n".join(log))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
