#!/usr/bin/env python3
"""Check/copy concise evidence from the three separate native-probe builds."""
import hashlib
import json
import re
from pathlib import Path
import shutil
import struct
import sys


def accessible(data):
    assert data[:4] == b"WCA1" and len(data) >= 16
    at, rows = 16, []
    while at < len(data):
        low, high, flags, depth, nl, nd = struct.unpack_from("<6I", data, at)
        at += 24
        assert nl + nd <= len(data) - at
        label = data[at:at + nl].decode("utf-8")
        at += nl
        detail = data[at:at + nd].decode("utf-8")
        at += nd
        rows.append(dict(flags=flags, label=label, detail=detail))
    assert at == len(data)
    return rows


build_root = Path(sys.argv[1] if len(sys.argv) > 1 else "build")
out = Path(__file__).resolve().parent / "results"
out.mkdir(exist_ok=True)
summary = {"scope": "Exact final GCC i386 OMF application engine objects, OpenWatcom Linux CRT and production native reactor/runtime/raster under QEMU Pentium. No OS/2 kernel or PM execution; memory_pages unsupported here falls back to System; deterministic test-only random callback.", "applications": {}}
for kind in ("word", "grid", "deck"):
    source = build_root / f"native-{kind}-probe"
    log = (source / "probe.log").read_text()
    assert "PASS native i386 engine init, eight frames" in log
    assert log.count("random_calls=1") == 8
    blank = accessible((source / f"{kind}-2.a11y").read_bytes())
    loaded = accessible((source / f"{kind}-7.a11y").read_bytes())
    blank_text = "\n".join(r["label"] + "\n" + r["detail"] for r in blank)
    loaded_text = "\n".join(r["label"] + "\n" + r["detail"] for r in loaded)
    if kind == "word":
        docs = [r for r in blank if r["flags"] & 16]
        assert docs and all(r["detail"].strip() == "" for r in docs)
        assert "Warp WordCraft" in loaded_text and "Café & résumé" in loaded_text
    elif kind == "grid":
        assert "100%" in blank_text and "100%" in loaded_text
        assert "Sales dashboard" not in blank_text and "Quarterly sales" not in blank_text
        assert "NORTHSTAR STUDIO   /   PILOT FORECAST" in loaded_text
    else:
        assert "Northern Lights" not in blank_text
        assert sum(bool(re.fullmatch(r"Button - Slide \d+", r["label"])) for r in blank) == 1
        assert sum(bool(re.fullmatch(r"Button - Slide \d+", r["label"])) for r in loaded) == 3
    shutil.copyfile(source / "probe.log", out / f"{kind}.log")
    record = {"blankAccessibleRows": len(blank), "fixtureAccessibleRows": len(loaded), "checksPassed": True, "files": {}}
    for frame in (2, 7):
        for suffix in ("a11y", "png"):
            name = f"{kind}-{frame}.{suffix}"
            shutil.copyfile(source / name, out / name)
            record["files"][name] = hashlib.sha256((out / name).read_bytes()).hexdigest()
    summary["applications"][kind] = record
    print(f"PASS {kind}: eight native frames, fixture open, raster, draw consumption and accessible logical-state checks")
(out / "SUMMARY.json").write_text(json.dumps(summary, indent=2) + "\n")
