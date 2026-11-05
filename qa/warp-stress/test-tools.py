#!/usr/bin/env python3
"""Host API-shim lifecycle/CLI/log tests; these do not execute an OS/2 kernel."""
import csv
import os
from pathlib import Path
import subprocess
import sys

source = Path(__file__).resolve().parent
build = Path(sys.argv[1]).resolve()
build.mkdir(parents=True, exist_ok=True)
for name in ("pressure", "memwatch"):
    subprocess.run(["gcc", "-std=c99", "-DWARP_STRESS_TEST", "-Wno-pointer-to-int-cast",
                    "-Wno-int-to-pointer-cast", "-Wall", "-Wextra", "-g",
                    "-fsanitize=address,undefined", str(source / f"{name}.c"),
                    str(source / "test-stubs/os2.c"), "-o", str(build / name)], check=True)

env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")

def run(tool, args, code=0, extra=None):
    process = subprocess.run([str(build / tool), *args], cwd=build,
                             env=dict(env, **(extra or {})), capture_output=True, text=True)
    assert process.returncode == code, (tool, args, process.returncode, process.stdout, process.stderr)
    assert "ERROR: AddressSanitizer" not in process.stderr
    assert "runtime error:" not in process.stderr
    return process

def rows(name):
    with (build / name).open(newline="") as f:
        reader = csv.DictReader(f)
        data = list(reader)
        assert all(None not in row and None not in row.values() for row in data), data
        return data

run("pressure", ["--mib", "257"], 2)
run("pressure", ["--mib", "-1"], 2)
run("pressure", ["--mib", "2", "--churn-mib", "3"], 2)
run("pressure", ["--seconds", "0"], 2)
run("pressure", ["--mib", "2", "--churn-mib", "1", "--seconds", "1", "--ramp-ms", "0",
                 "--interval-ms", "100", "--log", "steady.csv"])
data = rows("steady.csv")
assert data[-1]["event"] == "released" and int(data[-1]["owned_committed_bytes"]) == 0
assert any(row["event"] == "revisit" for row in data)
assert max(int(row["owned_committed_bytes"]) for row in data) == 2 * 1048576
run("pressure", ["--mib", "4", "--seconds", "1", "--ramp-ms", "0", "--log", "fail.csv"],
    1, {"WARP_TEST_FAIL_AT": "3"})
data = rows("fail.csv")
assert data[-1]["event"] == "released" and any(row["event"] == "allocation_failed" for row in data)
run("pressure", ["--mib", "2", "--log", "floor.csv"], 1, {"WARP_TEST_AVAILABLE_MIB": "16"})
assert rows("floor.csv")[-1]["owned_committed_bytes"] == "0"
run("pressure", ["--mib", "2", "--seconds", "1", "--ramp-ms", "0", "--log", "interrupt.csv"],
    0, {"WARP_TEST_INTERRUPT_MS": "150"})
assert rows("interrupt.csv")[-1]["event"] == "released"
assert int(rows("interrupt.csv")[-1]["elapsed_ms"]) < 1000
(build / "stop.now").write_text("stop\n")
run("pressure", ["--stop", "stop.now", "--log", "should-not-create.csv"])
assert not (build / "should-not-create.csv").exists()
run("memwatch", ["--seconds", "1", "--interval-ms", "100", "--log", "system.csv",
                 "--tasks", "tasks.csv", "--swap", "C:missing"])
data = rows("system.csv")
assert len(data) >= 2
assert data[0]["physical_total_bytes"] == str(128 * 1048576)
assert data[0]["free_physical_legacy_estimate_bytes"] == str(64 * 1048576)
assert data[0]["swap_drive_total_bytes"] == str(4 * 1024**3)
assert data[0]["swap_drive_free_bytes"] == str(2 * 1024**3)
assert rows("tasks.csv")[0]["query_rc"] == "1"
for mode in ("valid", "overflow", "invalid"):
    run("memwatch", ["--seconds", "1", "--log", f"{mode}-sys.csv", "--tasks", f"{mode}-tasks.csv"],
        0, {"WARP_TEST_TASKS": mode})
    data = rows(f"{mode}-tasks.csv")
    if mode == "invalid":
        assert data[0]["query_rc"] == "13"
    else:
        assert len(data) == 2
        assert data[0]["thread_system_counter_sum"] == "12"
        assert data[0]["thread_user_counter_sum"] == "20"
        assert data[0]["module"] == 'C:\\CraftTrio\\Word"Craft.EXE'
run("memwatch", ["--seconds", "1", "--no-legacy", "--log", "no-legacy.csv"])
assert rows("no-legacy.csv")[0]["free_physical_legacy_estimate_bytes"] == ""
print("PASS: bounded pressure, churn, allocation failure cleanup, available guard, stop file,")
print("      Ctrl-C cleanup, CLI limits, CSV columns, metric labels, task parsing/bounds/overflow,")
print("      unavailable task API, 64-bit disk totals.")
