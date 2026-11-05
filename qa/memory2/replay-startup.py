#!/usr/bin/env python3
"""Replay real rebuilt-engine packets against the memory1/new native renderers.

These are host WASI captures and software-renderer checks, not OS/2 screenshots.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path

from PIL import Image

KIT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("mask_qa", Path(__file__).with_name("test-mask-textures.py"))
qa = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qa)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("manifests", type=Path, nargs="+")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    libraries = [qa.build_library(args.baseline.resolve(), out, "replay-memory1"),
                 qa.build_library(KIT / "native/host", out, "replay-memory2")]
    report = {"scope": "Real host WASI packets; native C software rendering, not OS/2/GPI", "apps": []}
    for manifest_path in args.manifests:
        manifest_path = manifest_path.resolve()
        manifest = json.loads(manifest_path.read_text())
        kind = manifest["kind"]
        before, after = [qa.qa.Renderer(lib) for lib in libraries]
        frames = []
        try:
            for frame in manifest["frames"]:
                data = (manifest_path.parent / frame["file"]).read_bytes()
                assert hashlib.sha256(data).hexdigest() == frame["sha256"]
                width, height = frame["width"], frame["height"]
                assert before.frame(width, height, data), before.error()
                assert after.frame(width, height, data), after.error()
                pixels = after.pixels()
                assert before.pixels() == pixels, (kind, frame["sequence"], "pixel mismatch")
                old, new = qa.memory(before), qa.memory(after)
                frames.append({"sequence": frame["sequence"], "width": width, "height": height,
                               "baseline_texture_bytes": old["textures"],
                               "mask_texture_bytes": new["textures"],
                               "texture_bytes_saved": old["textures"] - new["textures"],
                               "pixels_sha256": hashlib.sha256(pixels[2]).hexdigest()})
                if frame["sequence"] in (8, 9, len(manifest["frames"])):
                    image = Image.frombytes("RGB", (width, height), pixels[2], "raw", "BGR")
                    image.save(out / f"{kind}-host-render-{frame['sequence']:03d}.png")
        finally:
            before.close()
            after.close()
        report["apps"].append({"kind": kind, "module_sha256": manifest.get("module_sha256"), "frames": frames})
        print(kind, len(frames), "real frames match; final texture saving", frames[-1]["texture_bytes_saved"], "bytes", flush=True)
    report["passed"] = True
    (out / "real-packet-replay.json").write_text(json.dumps(report, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
