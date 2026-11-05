#!/usr/bin/env python3
"""Build an explicit Craft Trio profile into a fresh directory with provenance.

Reconstructed source workflow. It does not assert that rebuilt binaries match a
previous release, and never marks an output as validated or publishes it.
Native EXEs are copied unchanged by default; iterdata2 packing is explicit.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

SUITE = Path(__file__).resolve().parents[1]
NATIVE = SUITE / "native"
TARGET = "wasm32-wasip1"
APPS = {
    "word": ("gui-upstream", "wordcraft-warp", "wordcraft_warp.wasm", "WORDCRFT.EXE"),
    "deck": ("deck-bridge", None, "deckcraft_warp.wasm", "DECKCRFT.EXE"),
    "grid": ("grid-upstream", "gridcraft-warp", "gridcraft_warp.wasm", "GRIDCRFT.EXE"),
}
PROFILE_NOTES = {
    "memory3": "Historical compiler configuration on current source; raw LX is the storage default, not an exact old binary rebuild.",
    "memory4": "Rust/native optimization configuration; raw LX is the storage default; every rebuild requires numerical, document and native OS/2 qualification.",
}
SKIP = {".git", "target", "build", "__pycache__", "node_modules"}


def digest(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


def snapshot(apps):
    roots = [NATIVE, SUITE / "icons", SUITE / "qa/rng"]
    if apps:
        roots += [SUITE / "patches", SUITE / "qa/native-engine/objconv-reloc-test.py"]
        roots += [SUITE / "gui-upstream", SUITE / "gui-craft-libs"] if "word" in apps else []
        roots += [SUITE / "grid-upstream", SUITE / "grid-craft-libs"] if "grid" in apps else []
        roots += [SUITE / "deck-bridge", SUITE / "deck-upstream", SUITE / "grid-craft-libs"] if "deck" in apps else []
    files = {}
    for root in roots:
        if not root.exists():
            raise RuntimeError("Required source input is missing: " + str(root))
        entries = [root] if root.is_file() else root.rglob("*")
        for path in entries:
            relative = path.relative_to(SUITE)
            if set(relative.parts) & SKIP:
                continue
            if path.is_symlink():
                files[relative.as_posix()] = {"symlink": os.readlink(path)}
            elif path.is_file():
                files[relative.as_posix()] = {"bytes": path.stat().st_size, "sha256": digest(path)}
    revisions = {}
    for directory in [SUITE, *(SUITE / APPS[app][0] for app in apps)]:
        top = subprocess.run(["git", "rev-parse", "--show-toplevel"], cwd=directory,
                             text=True, capture_output=True)
        if top.returncode == 0 and Path(top.stdout.strip()).resolve() == directory.resolve():
            revisions[str(directory.relative_to(SUITE))] = subprocess.check_output(
                ["git", "rev-parse", "HEAD"], cwd=directory, text=True).strip()
    return {"files": files, "revisions": revisions}


def capacity(path, minimum_bytes, minimum_inodes):
    free = shutil.disk_usage(path).free
    stat = os.statvfs(path)
    inodes = stat.f_favail
    # Some filesystems do not expose an inode limit and return zero totals.
    if free < minimum_bytes or (stat.f_files and inodes < minimum_inodes):
        raise RuntimeError("Build capacity floor reached: free_bytes=%d free_inodes=%d" % (free, inodes))
    return {"freeBytes": free, "freeInodes": inodes, "time": time.time()}


def tool_record(executable, version_args=None):
    found = shutil.which(executable)
    if not found:
        raise RuntimeError("Required tool not found: " + executable)
    path = Path(found).absolute()
    result = {"path": str(path), "resolvedPath": str(path.resolve()), "sha256": digest(path)}
    if version_args:
        version = subprocess.run([str(path), *version_args], text=True, capture_output=True, timeout=30)
        result.update(version=(version.stdout + version.stderr)[:4096], versionReturnCode=version.returncode)
    return result


def executable_path(value):
    found = shutil.which(value)
    if found:
        return str(Path(found).absolute())
    return str(Path(value).expanduser().resolve()) if os.sep in value else value


def run_step(step, environment, out, manifest, minimum_bytes, minimum_inodes, interval):
    record = {**step, "started": time.time(), "status": "running"}
    manifest["results"].append(record)
    log = out / "logs" / (step["label"] + ".log")
    record["log"] = str(log.relative_to(out))
    record["capacityBefore"] = capacity(out, minimum_bytes, minimum_inodes)
    write_json(out / "BUILD.json", manifest)
    child_env = dict(environment)
    child_env.update(step["env"])
    with log.open("wb") as output:
        process = subprocess.Popen(step["argv"], cwd=step["cwd"], env=child_env,
                                   stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            while True:
                try:
                    code = process.wait(timeout=interval)
                    break
                except subprocess.TimeoutExpired:
                    sample = capacity(out, minimum_bytes, minimum_inodes)
                    with (out / "capacity.jsonl").open("a") as stream:
                        stream.write(json.dumps({"step": step["label"], **sample}) + "\n")
        except BaseException:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            record.update(status="interrupted", finished=time.time())
            raise
    record.update(returnCode=code, finished=time.time(), status="passed" if code == 0 else "failed")
    if code:
        raise RuntimeError("Build step failed: " + step["label"] + "; see " + str(log))
    if step["expected"]:
        artifact = Path(step["expected"])
        if not artifact.is_file():
            record["status"] = "failed"
            raise RuntimeError("Build produced no expected output: " + str(artifact))
        record["artifact"] = {"path": str(artifact), "bytes": artifact.stat().st_size, "sha256": digest(artifact)}
    write_json(out / "BUILD.json", manifest)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", required=True, choices=tuple(PROFILE_NOTES))
    parser.add_argument("--app", default="all", choices=("all", "host", *APPS))
    parser.add_argument("--out", required=True, type=Path, help="fresh directory; existing outputs are refused")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--grid-native-opt", choices=("O0", "O1", "Os"),
                        help="override Grid native C optimization; defaults: memory3 O0, memory4 O1")
    parser.add_argument("--lx-packing", choices=("none", "iterdata2"),
                        help="EXE storage: default none (raw LX); iterdata2 is an explicit research/qualification opt-in")
    parser.add_argument("--plan", action="store_true", help="print commands without running tools or creating files")
    parser.add_argument("--min-free-gib", type=float, help="capacity floor; defaults: all8GiB, one engine4GiB, host128MiB")
    parser.add_argument("--min-free-inodes", type=int, default=20000)
    parser.add_argument("--capacity-interval", type=int, default=10, help="periodic capacity check in seconds (1..60)")
    args = parser.parse_args()
    if args.lx_packing is not None and args.app == "host":
        parser.error("--lx-packing requires --app all, word, deck, or grid")
    if args.grid_native_opt is not None and args.app not in ("all", "grid"):
        parser.error("--grid-native-opt requires --app grid or all")
    if not 1 <= args.jobs <= 32 or not 1 <= args.capacity_interval <= 60:
        parser.error("--jobs must be 1..32 and --capacity-interval 1..60")
    if args.min_free_inodes < 0 or (args.min_free_gib is not None and
            (not math.isfinite(args.min_free_gib) or args.min_free_gib <= 0)):
        parser.error("capacity floors must be positive (inode floor may be zero)")
    out = args.out.expanduser().resolve()
    if out.exists():
        parser.error("--out exists; use a fresh directory")
    if out.is_relative_to(SUITE) and out.relative_to(SUITE).parts[0] != "build":
        parser.error("Place outputs under the suite build directory or outside the source tree")
    apps = list(APPS) if args.app == "all" else [] if args.app == "host" else [args.app]
    lx_packing = (args.lx_packing or "none") if apps else None
    grid_native_opt = None
    if "grid" in apps:
        grid_native_opt = args.grid_native_opt or ("O1" if args.profile == "memory4" else "O0")
    minimum = args.min_free_gib if args.min_free_gib is not None else 8 if len(apps) == 3 else 4 if apps else .125
    minimum_bytes = int(minimum * 1024**3)
    cargo = executable_path(os.environ.get("CRAFT_CARGO", "cargo"))
    toolchain = os.environ.get("CRAFT_RUST_TOOLCHAIN", "1.95.0")
    cargo_prefix = [cargo, *( ["+" + toolchain] if toolchain else [] )]
    objconv = executable_path(os.environ.get("CRAFT_OBJCONV", "objconv"))
    steps = []

    def add(label, cwd, argv, env=None, expected=None):
        steps.append({"label": label, "cwd": str(cwd), "argv": [str(v) for v in argv],
                      "env": env or {}, "expected": str(expected) if expected else None})

    if apps:
        add("objconv-preflight", SUITE, [sys.executable, SUITE / "qa/native-engine/objconv-reloc-test.py",
                                        objconv, "--output", out / "qa/objconv"])
    for app in apps:
        directory, package, wasm_name, exe = APPS[app]
        release = app != "word" or args.profile == "memory4"
        rust_mode = "release" if release else "debug"
        settings = {"CARGO_TARGET_DIR": str(out / ("cargo-" + app)), "CARGO_INCREMENTAL": "0",
                    "CARGO_BUILD_JOBS": str(args.jobs)}
        if release:
            settings.update(CARGO_PROFILE_RELEASE_OPT_LEVEL="z", CARGO_PROFILE_RELEASE_DEBUG="0",
                            CARGO_PROFILE_RELEASE_LTO="thin", CARGO_PROFILE_RELEASE_CODEGEN_UNITS="16")
            if app == "word":
                settings.update(CARGO_PROFILE_RELEASE_CODEGEN_UNITS="1", CARGO_PROFILE_RELEASE_OVERFLOW_CHECKS="true")
            else:
                settings["CARGO_PROFILE_RELEASE_PANIC"] = "abort"
        else:
            settings.update(CARGO_PROFILE_DEV_OPT_LEVEL="1", CARGO_PROFILE_DEV_DEBUG="0")
        wasm = out / ("cargo-" + app) / TARGET / rust_mode / wasm_name
        command = cargo_prefix + ["rustc", "--offline", "--locked", *(["--release"] if release else []), "--target", TARGET]
        command += ["-p", package] if package else []
        command += ["--lib", "--", "-C", "strip=symbols"]
        add(app + "-rust", SUITE / directory, command, settings, wasm)
        optimization = grid_native_opt if app == "grid" else "Os"
        add(app + "-native", SUITE, ["bash", NATIVE / "build-engine.sh", wasm, out / app],
            {"CRAFT_ENGINE_OPT": optimization, "CRAFT_JOBS": str(args.jobs), "CRAFT_WORD_SUPPORT": str(NATIVE / "host")},
            out / app / "objects/word-meta.obj")
        add(app + "-link", SUITE, ["bash", NATIVE / "link-native.sh", app, out / app, out / "unpacked"],
            expected=out / "unpacked" / exe)
        if lx_packing == "iterdata2":
            add(app + "-pack", SUITE, [sys.executable, NATIVE / "pack-lx.py", out / "unpacked" / exe, out / "BIN" / exe],
                expected=out / "BIN" / exe)
        else:
            add(app + "-copy", SUITE, [sys.executable, NATIVE / "copy-native.py", out / "unpacked" / exe, out / "BIN" / exe],
                expected=out / "BIN" / exe)
    add("host", SUITE, ["bash", NATIVE / "link-native.sh", "host", out / "host", out / "BIN"], expected=out / "BIN/CRAFTPM.DLL")
    add("rng", SUITE, ["bash", SUITE / "qa/rng/build-probe.sh", "os2", out / "rng", out / "BIN"], expected=out / "BIN/RNGTEST.EXE")
    plan = {"profile": args.profile, "profileNote": PROFILE_NOTES[args.profile], "apps": apps,
            "output": str(out), "validationStatus": "not-run", "steps": steps,
            "gridNativeOpt": grid_native_opt, "gridNativeOptOverride": args.grid_native_opt,
            "lxPacking": lx_packing,
            "capacityFloor": {"freeBytes": minimum_bytes, "freeInodes": args.min_free_inodes,
                              "intervalSeconds": args.capacity_interval}}
    if args.plan:
        print(json.dumps(plan, indent=2))
        return 0
    if os.environ.get("RUSTFLAGS") or os.environ.get("CARGO_ENCODED_RUSTFLAGS") or any(
            name.endswith("_RUSTFLAGS") and value for name, value in os.environ.items() if name.startswith("CARGO_")):
        parser.error("Clear inherited Rust compiler flags before a recorded release build")
    if not os.environ.get("WATCOM"):
        parser.error("Set WATCOM to the OpenWatcom directory")
    for step in steps:
        script = Path(step["argv"][1]) if step["argv"][0] in ("bash", sys.executable) else None
        if not Path(step["cwd"]).is_dir() or (script is not None and not script.is_file()):
            parser.error("Required source/builder is absent for " + step["label"] + "; apply this overlay to a full source checkout")
    ancestor = out.parent
    while not ancestor.exists():
        ancestor = ancestor.parent
    capacity(ancestor, minimum_bytes, args.min_free_inodes)
    tools = {"python": tool_record(sys.executable), "bash": tool_record("bash"), "git": tool_record("git")}
    for name in ("wcc386", "wlink"):
        tools[name] = tool_record(str(Path(os.environ["WATCOM"]) / "binl64" / name))
    if apps:
        tools.update(cargo=tool_record(cargo, [*(["+" + toolchain] if toolchain else []), "--version"]),
                     cc=tool_record(os.environ.get("CRAFT_CC", "gcc"), ["--version"]),
                     wasm2c=tool_record(os.environ.get("CRAFT_WASM2C", "wasm2c"), ["--version"]),
                     objconv=tool_record(objconv), wrc=tool_record(os.environ.get("CRAFT_WRC", str(Path(os.environ["WATCOM"]) / "binl64/wrc"))))
        rustc = os.environ.get("RUSTC", "rustc")
        rustc_version = [*(["+" + toolchain] if toolchain and "RUSTC" not in os.environ else []), "--version", "--verbose"]
        tools["rustc"] = tool_record(rustc, rustc_version)
        if os.environ.get("RUSTC_WRAPPER"):
            tools["rustcWrapper"] = tool_record(os.environ["RUSTC_WRAPPER"])
    inputs = snapshot(apps)
    out.mkdir(parents=True)
    (out / "BIN").mkdir()
    (out / "logs").mkdir()
    manifest = {**plan, "status": "building", "started": time.time(), "tools": tools,
                "inputs": inputs, "results": []}
    write_json(out / "BUILD.json", manifest)
    environment = {key: value for key, value in os.environ.items() if not key.startswith("CARGO_PROFILE_")}
    environment["WATCOM"] = str(Path(environment["WATCOM"]).expanduser().resolve())
    if apps:
        for variable, name in [("CRAFT_OBJCONV", "objconv"), ("CRAFT_CC", "cc"),
                               ("CRAFT_WASM2C", "wasm2c"), ("CRAFT_WRC", "wrc")]:
            environment[variable] = tools[name]["path"]
        if environment.get("RUSTC"):
            environment["RUSTC"] = tools["rustc"]["path"]
        if environment.get("RUSTC_WRAPPER"):
            environment["RUSTC_WRAPPER"] = tools["rustcWrapper"]["path"]
    try:
        for step in steps:
            run_step(step, environment, out, manifest, minimum_bytes, args.min_free_inodes, args.capacity_interval)
        if snapshot(apps) != inputs:
            raise RuntimeError("Source inputs changed during the build")
        manifest["status"] = "built-unvalidated"
        manifest["binaries"] = [{"name": p.name, "bytes": p.stat().st_size, "sha256": digest(p)} for p in sorted((out / "BIN").iterdir()) if p.is_file()]
    except BaseException as error:
        manifest.update(status="failed", error=str(error))
        raise
    finally:
        manifest["finished"] = time.time()
        write_json(out / "BUILD.json", manifest)
    print(str(out / "BUILD.json"))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print("Build stopped: " + str(error), file=sys.stderr)
        raise SystemExit(1)
