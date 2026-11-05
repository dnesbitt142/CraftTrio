#!/usr/bin/env python3
"""Exercise LX storage plans and the real raw-copy branch without an SDK."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

SUITE = Path(__file__).resolve().parents[2]
BUILDER = SUITE / "native/release-build.py"


class LxPackingTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="craft-lx-policy-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.index = 0
        self.environment = dict(os.environ)
        for name in ("CRAFT_CARGO", "CRAFT_CC", "CRAFT_OBJCONV", "CRAFT_WASM2C", "CRAFT_WRC", "WATCOM"):
            self.environment[name] = str(self.root / ("unavailable-" + name))

    def plan(self, profile="memory4", app="all", packing=None, extra=(), success=True):
        self.index += 1
        output = self.root / "absent-parent" / ("output-" + str(self.index))
        argv = [sys.executable, str(BUILDER), "--profile", profile, "--app", app,
                "--out", str(output), "--plan", *extra]
        if packing is not None:
            argv += ["--lx-packing", packing]
        before = sorted(self.root.rglob("*"))
        run = subprocess.run(argv, cwd=self.root, env=self.environment,
                             capture_output=True, text=True, timeout=15)
        self.assertEqual(before, sorted(self.root.rglob("*")), "plan created build outputs")
        if not success:
            self.assertNotEqual(run.returncode, 0)
            self.assertEqual(run.stdout, "")
            return run.stderr
        self.assertEqual(run.returncode, 0, run.stderr)
        plan = json.loads(run.stdout)
        self.assertEqual(plan["validationStatus"], "not-run")
        return plan

    @staticmethod
    def normalize(plan):
        output = plan["output"]

        def visit(value):
            if isinstance(value, str):
                return value.replace(output, "<OUTPUT>")
            if isinstance(value, list):
                return [visit(item) for item in value]
            if isinstance(value, dict):
                return {key: visit(item) for key, item in value.items()}
            return value

        return visit(plan)

    def test_default_is_raw_for_every_engine_scope_and_profile(self):
        for profile in ("memory3", "memory4"):
            for app in ("all", "word", "deck", "grid"):
                with self.subTest(profile=profile, app=app):
                    plan = self.plan(profile, app)
                    self.assertEqual(plan["lxPacking"], "none")
                    self.assertFalse(any(step["label"].endswith("-pack") for step in plan["steps"]))
                    copies = [step for step in plan["steps"] if step["label"].endswith("-copy")]
                    self.assertEqual(len(copies), 3 if app == "all" else 1)
                    for step in copies:
                        self.assertEqual(Path(step["argv"][1]).name, "copy-native.py")
                        self.assertEqual(step["argv"][-1], step["expected"])
                    explicit = self.plan(profile, app, "none")
                    self.assertEqual(self.normalize(plan), self.normalize(explicit))

    def test_iterdata2_is_explicit_and_preserves_other_build_steps(self):
        for profile in ("memory3", "memory4"):
            for app in ("all", "word", "deck", "grid"):
                with self.subTest(profile=profile, app=app):
                    expected = self.normalize(self.plan(profile, app))
                    actual = self.normalize(self.plan(profile, app, "iterdata2"))
                    expected["lxPacking"] = "iterdata2"
                    for step in expected["steps"]:
                        if step["label"].endswith("-copy"):
                            step["label"] = step["label"][:-5] + "-pack"
                            step["argv"][1] = str(Path(step["argv"][1]).with_name("pack-lx.py"))
                    self.assertEqual(actual, expected)

    def test_grid_choice_and_host_only_contracts(self):
        for level in ("O0", "O1", "Os"):
            for packing in ("none", "iterdata2"):
                with self.subTest(level=level, packing=packing):
                    plan = self.plan(packing=packing, extra=("--grid-native-opt", level))
                    grid = next(step for step in plan["steps"] if step["label"] == "grid-native")
                    self.assertEqual(grid["env"]["CRAFT_ENGINE_OPT"], level)
                    self.assertEqual(plan["gridNativeOpt"], level)
                    self.assertEqual(plan["lxPacking"], packing)
        host = self.plan(app="host")
        self.assertIsNone(host["lxPacking"])
        self.assertEqual([step["label"] for step in host["steps"]], ["host", "rng"])
        for packing in ("none", "iterdata2"):
            self.assertIn("requires --app", self.plan(app="host", packing=packing, success=False))
        self.plan(packing="lzma", success=False)

    def test_real_planned_copy_preserves_bytes_mode_and_hashes(self):
        plan = self.plan(app="word")
        step = next(item for item in plan["steps"] if item["label"] == "word-copy")
        source, destination = map(Path, step["argv"][-2:])
        source.parent.mkdir(parents=True)
        data = b"MZ" + bytes(range(256)) * 263 + b"\0LX\0\xff"
        source.write_bytes(data)
        source.chmod(0o755)
        self.assertFalse(destination.parent.exists())
        run = subprocess.run(step["argv"], cwd=self.root, capture_output=True, text=True, timeout=15)
        self.assertEqual(run.returncode, 0, run.stderr)
        report = json.loads(run.stdout)
        expected = hashlib.sha256(data).hexdigest()
        self.assertEqual(source.read_bytes(), data)
        self.assertEqual(destination.read_bytes(), data)
        self.assertEqual(report["bytes"], len(data))
        self.assertEqual(report["sha256"], expected)
        self.assertEqual(report["sourceSha256"], expected)
        self.assertEqual(report["method"], "byte-preserving-copy")
        self.assertEqual(stat.S_IMODE(destination.stat().st_mode), stat.S_IMODE(source.stat().st_mode))
        self.assertEqual(list(destination.parent.glob(".native-copy-*")), [])

    def test_copy_cli_rejects_bad_inputs_without_clobbering(self):
        helper = BUILDER.with_name("copy-native.py")
        source = self.root / "input.exe"
        source.write_bytes(b"original binary bytes")
        run = subprocess.run([sys.executable, str(helper), str(source), str(source)],
                             capture_output=True, text=True, timeout=15)
        self.assertNotEqual(run.returncode, 0)
        self.assertEqual(source.read_bytes(), b"original binary bytes")
        destination = self.root / "absent-output" / "BIN" / "missing.exe"
        run = subprocess.run([sys.executable, str(helper), str(self.root / "missing.exe"), str(destination)],
                             capture_output=True, text=True, timeout=15)
        self.assertNotEqual(run.returncode, 0)
        self.assertFalse(destination.parent.exists())

    def test_changed_source_does_not_promote_partial_copy(self):
        spec = importlib.util.spec_from_file_location("copy_native", BUILDER.with_name("copy-native.py"))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        source, destination = self.root / "source.exe", self.root / "destination.exe"
        source.write_bytes(b"linked binary")
        destination.write_bytes(b"keep existing output")
        real_copy = module.shutil.copy2

        def copy_then_change(*args, **kwargs):
            result = real_copy(*args, **kwargs)
            source.write_bytes(b"changed during copy")
            return result

        with mock.patch.object(module.shutil, "copy2", side_effect=copy_then_change):
            with self.assertRaisesRegex(RuntimeError, "source changed"):
                module.copy_native(source, destination)
        self.assertEqual(destination.read_bytes(), b"keep existing output")
        self.assertEqual(list(self.root.glob(".native-copy-*")), [])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, add_help=False)
    parser.add_argument("--builder", type=Path, default=BUILDER)
    args, remaining = parser.parse_known_args()
    BUILDER = args.builder.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
