#!/usr/bin/env python3
"""Simulate native sessions/log arrival; does not execute OS/2 or PM."""
import csv
import os
from pathlib import Path
import subprocess
import sys

source=Path(__file__).resolve().parent
build=Path(sys.argv[1]).resolve()
build.mkdir(parents=True,exist_ok=True)
subprocess.run(["gcc","-std=c99","-DWARP_STRESS_TEST","-g","-fsanitize=address,undefined",
                "-Wall","-Wextra","-Wno-missing-field-initializers",str(source/"runtrio.c"),
                str(source/"test-stubs/runtrio-os2.c"),"-o",str(build/"runtrio")],check=True)
for mode, expected in (("ready",0),("logfail",1),("launchfail",1),("timeout",1),("clock",1),("stale",2),("setfhfail",2)):
    work=build/mode
    work.mkdir(exist_ok=True)
    for name in ("WCGUI.LOG","DCGUI.LOG","GCGUI.LOG","result.csv"):
        (work/name).unlink(missing_ok=True)
    if mode=="stale":(work/"WCGUI.LOG").write_text("Startup frame: 3, stale\n")
    result=subprocess.run([str(build/"runtrio"),"C:\\APP","result.csv","3"],cwd=work,
        env=dict(os.environ,RUNTRIO_TEST_MODE=mode),capture_output=True,text=True)
    assert result.returncode==expected,(mode,result.returncode,result.stdout,result.stderr)
    assert "ERROR: AddressSanitizer" not in result.stderr and "runtime error:" not in result.stderr
    if mode=="stale":
        assert not (work/"result.csv").exists()
        assert (work/"WCGUI.LOG").read_text().endswith("stale\n")
        continue
    if mode=="setfhfail":
        assert b"DosSetFHState): 87" in result.stderr.encode()
        assert (work/"result.csv").stat().st_size==0
        continue
    with (work/"result.csv").open(newline="") as f:
        data=list(csv.DictReader(f))
    assert all(None not in r and None not in r.values() for r in data)
    if mode=="clock":continue
    assert len({r["common_start_ms"] for r in data})==1
    launches=data[:3]
    assert all(r["event"] in ("launched","launch_failed") for r in launches)
    assert all(int(r["launch_return_elapsed_ms"])<100 for r in launches)
    assert launches[1]["start_rc"] in ("2","457")
    final=data[-3:]
    if mode=="ready":
        assert all(r["event"]=="complete" and r["ready_elapsed_ms"] for r in final)
        assert [r["pid"] for r in final]==["100","101","102"]
        assert [r["session"] for r in final]==["20","21","22"]
    if mode=="timeout":assert final[2]["event"]=="timeout" and final[2]["ready_elapsed_ms"]==""
    if mode=="logfail":assert final[1]["event"]=="failed" and final[1]["detail"].startswith("Could not")
    if mode=="launchfail":assert final[1]["event"]=="failed" and final[1]["start_rc"]=="2"
for timeout in ("0","601","-1","3x",""):
    result=subprocess.run([str(build/"runtrio"),"C:\\APP","unused.csv",timeout],cwd=build,capture_output=True)
    assert result.returncode==2,(timeout,result.stderr)
print("PASS: common timestamp, three adjacent PM starts, background success rc457, split-line readiness,")
print("      fresh-log rejection, launch/log/clock failures, timeout, CSV, numeric bounds, handle cleanup,")
print("      noinherit-only SetFHState flags, reserved access/share-bit rejection, setter failure.")
