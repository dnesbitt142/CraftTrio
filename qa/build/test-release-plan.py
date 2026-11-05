#!/usr/bin/env python3
"""Check release CLI plans without Cargo, compilers, an SDK or engine sources.

Run from any directory. --builder can select a candidate release-build.py.
The tests use private temporary paths and verify that every plan or rejected
command leaves those paths unchanged. They never request an actual build.
"""
import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BUILDER = Path(__file__).resolve().parents[2] / "native/release-build.py"


class ReleasePlanTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="craft-release-plan-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.counter = 0
        self.environment = dict(os.environ)
        # A plan must remain usable before any real toolchain is installed.
        for name in ("CRAFT_CARGO", "CRAFT_CC", "CRAFT_OBJCONV", "CRAFT_WASM2C", "CRAFT_WRC", "WATCOM"):
            self.environment[name] = str(self.root / ("unavailable-" + name))
        self.environment["CRAFT_RUST_TOOLCHAIN"] = "1.95.0"

    def invoke(self, profile="memory4", app="all", extra=(), success=True, output=None):
        self.counter += 1
        output = output or self.root / "absent-parent" / ("output-" + str(self.counter))
        before = sorted(str(path.relative_to(self.root)) for path in self.root.rglob("*"))
        process = subprocess.run([
            sys.executable, str(BUILDER), "--profile", profile, "--app", app,
            "--out", str(output), "--plan", *extra], cwd=self.root,
            env=self.environment, capture_output=True, text=True, timeout=15)
        after = sorted(str(path.relative_to(self.root)) for path in self.root.rglob("*"))
        self.assertEqual(before, after, "plan or rejected arguments changed the output tree")
        if success:
            self.assertEqual(process.returncode, 0, process.stderr)
            result = json.loads(process.stdout)
            self.assertEqual(result["validationStatus"], "not-run")
            return result
        self.assertNotEqual(process.returncode, 0, "invalid command unexpectedly succeeded")
        self.assertEqual(process.stdout, "", "invalid command emitted a successful plan")
        return process.stderr

    @staticmethod
    def step(plan, label):
        return next(step for step in plan["steps"] if step["label"] == label)

    @staticmethod
    def normalized(plan):
        """Remove only the unique output path before comparing complete plans."""
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

    def test_profile_defaults_and_preflight_order(self):
        for profile, expected in (("memory3", "O0"), ("memory4", "O1")):
            for app in ("all", "grid"):
                with self.subTest(profile=profile, app=app):
                    plan = self.invoke(profile, app)
                    self.assertEqual(plan["gridNativeOpt"], expected)
                    self.assertIsNone(plan["gridNativeOptOverride"])
                    self.assertEqual(self.step(plan, "grid-native")["env"]["CRAFT_ENGINE_OPT"], expected)
                    self.assertEqual(plan["steps"][0]["label"], "objconv-preflight")
                    self.assertEqual(plan["steps"][-2]["label"], "host")
                    self.assertEqual(plan["steps"][-1]["label"], "rng")
                    self.assertEqual(plan["capacityFloor"]["freeBytes"], (8 if app == "all" else 4) * 1024**3)

    def test_every_override_changes_only_grid_native_selection(self):
        for profile in ("memory3", "memory4"):
            for app in ("all", "grid"):
                baseline = self.normalized(self.invoke(profile, app))
                for level in ("O0", "O1", "Os"):
                    with self.subTest(profile=profile, app=app, level=level):
                        plan = self.normalized(self.invoke(profile, app, ("--grid-native-opt", level)))
                        expected = copy.deepcopy(baseline)
                        expected.update(gridNativeOpt=level, gridNativeOptOverride=level)
                        self.step(expected, "grid-native")["env"]["CRAFT_ENGINE_OPT"] = level
                        self.assertEqual(plan, expected)

    def test_existing_word_deck_and_grid_rust_settings(self):
        for profile in ("memory3", "memory4"):
            with self.subTest(profile=profile):
                plan = self.invoke(profile)
                for app in ("word", "deck"):
                    self.assertEqual(self.step(plan, app + "-native")["env"]["CRAFT_ENGINE_OPT"], "Os")
                word = self.step(plan, "word-rust")
                if profile == "memory3":
                    self.assertNotIn("--release", word["argv"])
                    self.assertEqual(word["env"]["CARGO_PROFILE_DEV_OPT_LEVEL"], "1")
                    self.assertEqual(word["env"]["CARGO_PROFILE_DEV_DEBUG"], "0")
                else:
                    self.assertIn("--release", word["argv"])
                    self.assertEqual(word["env"]["CARGO_PROFILE_RELEASE_OPT_LEVEL"], "z")
                    self.assertEqual(word["env"]["CARGO_PROFILE_RELEASE_DEBUG"], "0")
                    self.assertEqual(word["env"]["CARGO_PROFILE_RELEASE_LTO"], "thin")
                    self.assertEqual(word["env"]["CARGO_PROFILE_RELEASE_CODEGEN_UNITS"], "1")
                    self.assertEqual(word["env"]["CARGO_PROFILE_RELEASE_OVERFLOW_CHECKS"], "true")
                for app in ("deck", "grid"):
                    rust = self.step(plan, app + "-rust")
                    self.assertIn("--release", rust["argv"])
                    self.assertEqual(rust["env"]["CARGO_PROFILE_RELEASE_OPT_LEVEL"], "z")
                    self.assertEqual(rust["env"]["CARGO_PROFILE_RELEASE_DEBUG"], "0")
                    self.assertEqual(rust["env"]["CARGO_PROFILE_RELEASE_LTO"], "thin")
                    self.assertEqual(rust["env"]["CARGO_PROFILE_RELEASE_CODEGEN_UNITS"], "16")
                    self.assertEqual(rust["env"]["CARGO_PROFILE_RELEASE_PANIC"], "abort")
                for app in ("word", "deck", "grid"):
                    argv = self.step(plan, app + "-rust")["argv"]
                    self.assertIn("--offline", argv)
                    self.assertIn("--locked", argv)
                    self.assertEqual(argv[argv.index("--target") + 1], "wasm32-wasip1")

    def test_non_grid_plans_and_host_only_build_order(self):
        for app in ("word", "deck", "host"):
            with self.subTest(app=app):
                plan = self.invoke(app=app)
                self.assertIsNone(plan["gridNativeOpt"])
                self.assertIsNone(plan["gridNativeOptOverride"])
                if app == "host":
                    self.assertEqual([step["label"] for step in plan["steps"]], ["host", "rng"])
                    self.assertEqual(plan["capacityFloor"]["freeBytes"], 128 * 1024**2)

    def test_override_requires_grid_and_rejects_unknown_levels(self):
        for app in ("word", "deck", "host"):
            with self.subTest(app=app):
                error = self.invoke(app=app, extra=("--grid-native-opt", "Os"), success=False)
                self.assertIn("requires --app grid or all", error)
        for level in ("O2", "O3", "os", "-O1"):
            with self.subTest(level=level):
                self.invoke(extra=("--grid-native-opt=" + level,), success=False)

    def test_existing_argument_guards_leave_no_outputs(self):
        self.invoke(profile="unknown", success=False)
        self.invoke(app="unknown", success=False)
        for extra in (("--jobs", "0"), ("--jobs", "33"), ("--capacity-interval", "0"),
                      ("--capacity-interval", "61"), ("--min-free-gib", "0"),
                      ("--min-free-gib", "nan"), ("--min-free-inodes", "-1")):
            with self.subTest(extra=extra):
                self.invoke(extra=extra, success=False)
        existing = self.root / "existing-output"
        existing.mkdir()
        sentinel = existing / "keep.txt"
        sentinel.write_text("existing build stays intact\n")
        self.invoke(output=existing, success=False)
        self.assertEqual(sentinel.read_text(), "existing build stays intact\n")
        forbidden = BUILDER.parent.parent / (self.root.name + "-outside-build")
        self.assertFalse(forbidden.exists())
        error = self.invoke(output=forbidden, success=False)
        self.assertIn("Place outputs under the suite build directory", error)
        self.assertFalse(forbidden.exists())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, add_help=False)
    parser.add_argument("--builder", type=Path, default=BUILDER)
    arguments, remaining = parser.parse_known_args()
    BUILDER = arguments.builder.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
