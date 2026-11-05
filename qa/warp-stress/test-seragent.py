#!/usr/bin/env python3
"""Serial framing/partial-I/O/command-bound tests using native-API shims."""
import importlib.util
import os
import re
from pathlib import Path
import subprocess
import sys

source=Path(__file__).resolve().parent
build=Path(sys.argv[1]).resolve();build.mkdir(parents=True,exist_ok=True)
subprocess.run(["gcc","-std=c99","-DWARP_STRESS_TEST","-DSERAGENT_MAX_SEQUENCE=10",
                "-DSERAGENT_MAX_OUTPUT=5","-g","-fsanitize=address,undefined",
                "-Wall","-Wextra",str(source/"seragent.c"),str(source/"test-stubs/serial-os2.c"),
                "-o",str(build/"seragent")],check=True)
spec=importlib.util.spec_from_file_location("guestdecode",source/"decode-guestout.py")
decoder=importlib.util.module_from_spec(spec);spec.loader.exec_module(decoder)
blob=bytes(range(256))*4+b"tail\0\xff"
for path in build.iterdir():
    if re.fullmatch(r'C:\\CRAFT\\A[0-9]{7}\.OUT',path.name):path.unlink()
(build/"C:\\CRAFT\\BINARY.BIN").write_bytes(blob)
(build/"C:\\CRAFT\\EMPTY.BIN").write_bytes(b"")
commands=(b"\r\nEXEC echo owned\r\nGET C:\\CRAFT\\BINARY.BIN\nGET \"C:\\CRAFT\\EMPTY.BIN\"\r"
          b"GET C:\\CRAFT\\MISSING.BIN\nEXEC fail_owned_command\nGET relative.bin\nBOGUS\n"
          b"EXEC "+b"x"*513+b"\nEXEC bad\x01command\nEXIT\r\n")
result=subprocess.run([str(build/"seragent"),"115200"],input=commands,cwd=build,capture_output=True)
assert result.returncode==0,(result.returncode,result.stderr)
assert b"ERROR: AddressSanitizer" not in result.stderr and b"runtime error:" not in result.stderr
assert result.stdout.startswith(b"SERAGENT READY COM1 115200\r\n")
assert b"SERAGENT DONE 10 0 " in result.stdout
assert b"SERAGENT DONE 5 7 " in result.stdout
assert b"SERAGENT ACK 8 INVALID" in result.stdout
assert b"SERAGENT ACK 9 INVALID" in result.stdout
destination=build/"decoded"
destination.mkdir(exist_ok=True)
for old in destination.iterdir():old.unlink()
records=decoder.decode(result.stdout,destination)
assert len(records)==5,records
assert Path(records[0]["path"]).read_bytes()==b"executed stdout\nexecuted stderr\n"
assert Path(records[1]["path"]).read_bytes()==blob
assert Path(records[2]["path"]).read_bytes()==b""
assert records[3]["error"]
assert Path(records[4]["path"]).read_bytes().startswith(b"executed stdout")
assert records[0]["guest_path"]=="C:\\CRAFT\\A0000001.OUT"
assert records[4]["guest_path"]=="C:\\CRAFT\\A0000002.OUT"
previous={p.name:p.read_bytes() for p in build.iterdir() if re.fullmatch(r'C:\\CRAFT\\A[0-9]{7}\.OUT',p.name)}
# A restarted agent skips old captures; the two simulated children retain their streams.
restart=subprocess.run([str(build/"seragent")],input=b"EXEC start first\nEXEC start second\nEXIT\n",
                       cwd=build,capture_output=True)
assert restart.returncode==0,restart.stderr
restart_dir=build/"restart-decoded";restart_dir.mkdir(exist_ok=True)
for old in restart_dir.iterdir():old.unlink()
new_records=decoder.decode(restart.stdout,restart_dir)
assert [r["guest_path"] for r in new_records]==["C:\\CRAFT\\A0000003.OUT","C:\\CRAFT\\A0000004.OUT"]
assert all((build/name).read_bytes()==content for name,content in previous.items())
reserve_limit=subprocess.run([str(build/"seragent")],input=b"EXEC echo fifth\nEXEC echo unavailable\nEXIT\n",
                            cwd=build,capture_output=True)
assert reserve_limit.returncode==0,reserve_limit.stderr
assert b"SERAGENT ERROR 2 output_reservation_rc_4" in reserve_limit.stdout
assert b"SERAGENT DONE 2 1 " in reserve_limit.stdout
sequence_limit=subprocess.run([str(build/"seragent")],input=b"GET relative.bin\n"*11,
                             cwd=build,capture_output=True)
assert sequence_limit.returncode==1,sequence_limit.stderr
assert b"SERAGENT ERROR 0 sequence_limit_restart_agent" in sequence_limit.stdout
assert b"SERAGENT DONE 0 1 0" in sequence_limit.stdout
for mode in ("SERIAL_TEST_CONFIG_FAIL","SERIAL_TEST_WRITE_FAIL"):
    failed=subprocess.run([str(build/"seragent")],input=b"EXIT\n",cwd=build,
                          env=dict(os.environ,**{mode:"1"}),capture_output=True)
    assert failed.returncode==1,(mode,failed.stderr)
bad=subprocess.run([str(build/"seragent"),"9600"],capture_output=True)
assert bad.returncode==2
print("PASS: COM noinherit/8N1/no-flow/finite timeouts, 38400/115200 setup, fragmented commands,")
print("      partial writes, EXEC output/exit status, binary/empty/missing GET, original decoder,")
print("      distinct inherited child streams, restart-safe atomic capture names, preserved files,")
print("      bounded output/sequence exhaustion, 512-byte bound, invalid ASCII, errors, EXIT, cleanup.")
