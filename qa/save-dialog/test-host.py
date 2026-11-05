#!/usr/bin/env python3
"""Test extracted production startup/dispatch code; no OS/2 ABI/FOC claim.

Pass --baseline-pm to prove an older host fails the same modal replay test.
An actual native Warp/ArcaOS build needs the complete kit and OpenWatcom SDK.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import os
import subprocess


def function(source, name):
    match = re.search(r"^static [^\n]*\b" + name + r"\([^;]*?\)\s*\{", source, re.M)
    if not match:
        raise ValueError("Missing production function " + name)
    opening = source.index("{", match.start())
    # These selected functions have no brace literals; balance their C bodies.
    depth = 1
    position = opening + 1
    while depth:
        if source[position] == "{": depth += 1
        elif source[position] == "}": depth -= 1
        position += 1
    return source[match.start():position] + "\n"


def prepare(source_path, output):
    source = source_path.read_text()
    output.mkdir(parents=True, exist_ok=True)
    begin = source.index("static struct App {")
    end = source.index("} app;", begin) + len("} app;")
    defines = "\n".join(re.findall(r"^#define WM_WCG_(?:START|PICK_INPUT) .*$", source, re.M))
    tokens = "\n".join(re.findall(r"^static const char (?:start|input)_message_token[^\n]*", source, re.M))
    (output / "app.inc").write_text(defines + "\n" + source[begin:end] + "\n" + tokens + "\n")
    for name, filename in [("invoke", "invoke.inc"), ("start", "start.inc"), ("after_frame", "after-frame.inc")]:
        (output / filename).write_text(function(source, name))
    dispatch = function(source, "client_proc")
    begin = dispatch.index("    case WM_WCG_START:")
    end = dispatch.index("    case WM_PAINT:", begin)
    (output / "dispatch.inc").write_text(
        "static MRESULT client_proc(HWND window, ULONG msg, MPARAM mp1, MPARAM mp2) {\n"
        "    switch (msg) {\n" + dispatch[begin:end] + "    }\n"
        "    return WinDefWindowProc(window, msg, mp1, mp2);\n}\n")
    run = function(source, "run_app")
    begin = run.index("WinPostMsg(app.client, WM_WCG_START,")
    opening = run.index("(", begin)
    depth = 1
    end = opening + 1
    while depth:
        if run[end] == "(": depth += 1
        elif run[end] == ")": depth -= 1
        end += 1
    (output / "post-start.inc").write_text("    " + run[begin:end] + ";\n")
    return hashlib.sha256(source_path.read_bytes()).hexdigest()


def main():
    suite = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=suite / "build/save-dialog")
    parser.add_argument("--baseline-pm", type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    report = {"scope": "extracted production startup/invoke/message cases under reentrant host shims; not native OS/2 or document I/O", "runs": []}
    modes = [("fixed", suite / "native/host/pm.c", True)]
    if args.baseline_pm: modes.append(("baseline", args.baseline_pm.resolve(), False))
    for label, source, sanitized in modes:
        build = output / label
        sha = prepare(source, build)
        executable = build / "probe"
        command = shlex.split(os.environ.get("CC", "cc")) + ["-std=c99", "-g", "-O1", "-I" + str(build), "-I" + str(suite / "native/host")]
        if sanitized: command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
        command += [str(suite / "qa/save-dialog/host-regression.c"), "-o", str(executable)]
        subprocess.run(command, check=True, timeout=60)
        cases = ["modal-save", "nested-rng", "nested-start", "start-failure", "init-failure", "rng-failure", "pre-busy", "pre-failed", "pre-ready"] if sanitized else ["modal-save"]
        for case in cases:
            run = subprocess.run([str(executable), case], capture_output=True, text=True, timeout=15)
            text = run.stdout + run.stderr
            passed = (run.returncode == 0 and "PASS " + case in text and "runtime error:" not in text and "ERROR: AddressSanitizer" not in text) if sanitized else (run.returncode != 0 and "application engine is already running" in text)
            report["runs"].append({"mode": label, "case": case, "sourceSha256": sha, "passed": passed, "returncode": run.returncode, "stdout": run.stdout, "stderr": run.stderr})
            print(("PASS " if passed else "FAIL ") + label + " " + case)
    report["passed"] = all(row["passed"] for row in report["runs"])
    output.mkdir(parents=True, exist_ok=True)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
