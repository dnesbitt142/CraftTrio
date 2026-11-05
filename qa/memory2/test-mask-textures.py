#!/usr/bin/env python3
"""Check exact R8 coverage-cache behavior against the memory1 RGBA renderer.

Counts requested allocation bytes, not OS/2 process memory. The wire format and
pixel output must match the baseline for every accepted/rejected packet.
"""
import argparse
import ctypes as C
import importlib.util
import json
import os
from pathlib import Path
import random
import struct
import subprocess

KIT = Path(__file__).resolve().parents[2]
OLD_QA = KIT / "qa/memory"
spec = importlib.util.spec_from_file_location("memory1_qa", OLD_QA / "test-renderer.py")
qa = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qa)


def build_library(host, build, name):
    flags = [os.environ.get("CC", "gcc"), "-std=c99", "-O0", "-g", "-fPIC", "-I", str(host)]
    objects = []
    for source in [host / "raster.c", host / "surface.c"]:
        obj = build / (name + "-" + source.stem + ".o")
        subprocess.run([*flags, "-include", str(OLD_QA / "alloc-shim.h"),
                        "-c", str(source), "-o", str(obj)], check=True)
        objects.append(obj)
    alloc = build / (name + "-alloc.o")
    subprocess.run([*flags, "-c", str(OLD_QA / "alloc-shim.c"), "-o", str(alloc)], check=True)
    result = build / (name + ".so")
    subprocess.run([flags[0], "-shared", "-Wl,-Bsymbolic", *map(str, objects),
                    str(alloc), "-o", str(result)], check=True)
    return result


def memory(renderer):
    # First three fields are stable in memory1 and memory2. Leave room for
    # diagnostic additions without making the test depend on private layout.
    values = (C.c_size_t * 32)()
    renderer.lib.wc_raster_memory.argtypes = [C.c_void_p, C.c_void_p]
    renderer.lib.wc_raster_memory(renderer.instance, values)
    return dict(zip(["surface", "vertices", "textures"], values[:3]))


def mask(values):
    return b"".join(bytes([v]) * 4 for v in values)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--build", type=Path, default=KIT.parent / "build/memory2-tests")
    args = parser.parse_args()
    build = args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    libraries = [build_library(args.baseline.resolve(), build, "memory1"),
                 build_library(KIT / "native/host", build, "memory2")]
    before, after = [qa.Renderer(lib) for lib in libraries]
    checks = 0
    randomizer = random.Random(0x4d41534b)

    def compare(data, width=31, height=17, expected=True):
        nonlocal checks
        snapshots = before.pixels(), after.pixels()
        a, b = before.frame(width, height, data), after.frame(width, height, data)
        assert a == b, (a, b, before.error(), after.error())
        if expected is not None:
            assert a == expected, (before.error(), after.error())
        if a:
            assert before.pixels() == after.pixels(), "pixel mismatch"
        else:
            assert before.pixels() == snapshots[0], "baseline image changed on rejection"
            assert after.pixels() == snapshots[1], "candidate image changed on rejection"
        checks += 1

    try:
        coverage = mask([(i * 37) & 255 for i in range(64)])
        quad = qa.quad(1, 31, 17, 0xc8785010)
        compare(qa.packet([qa.upload(1, 8, 8, coverage)], [quad]))
        assert memory(before)["textures"] == 8 * 8 * 4
        assert memory(after)["textures"] == 8 * 8
        compare(qa.packet([qa.upload(1, 3, 2, mask([0, 17, 255, 128, 1, 254]), 2, 3)], [quad]))
        assert memory(after)["textures"] == 64
        compare(qa.packet([qa.upload(1, 1, 1, bytes([50, 20, 0, 80]), 4, 4)], [quad]))
        assert memory(after)["textures"] == 256, "colored partial update must promote the whole cache"
        compare(qa.packet([qa.upload(1, 1, 1, mask([96]), 2, 2)], [quad]))
        compare(qa.packet([qa.upload(1, 8, 8, coverage)], [quad]))
        assert memory(after)["textures"] == 64, "full mask update must compact an old RGBA texture"
        print("PASS mask sampling, colored promotion, partial updates and full recompaction", flush=True)

        # RGB equality alone is insufficient: alpha must also match exactly.
        colors = [(128, 128, 128, 255), (80, 80, 80, 81),
                  (80, 81, 80, 80), (80, 80, 81, 80), (81, 80, 80, 80)]
        for color in colors:
            compare(qa.packet([qa.upload(1, 1, 1, bytes(color))], [quad]))
            assert memory(after)["textures"] == 4
        for value in [0, 1, 17, 128, 254, 255]:
            compare(qa.packet([qa.upload(1, 1, 1, mask([value]))], [quad]))
            assert memory(after)["textures"] == 1
        print("PASS exact R=G=B=A eligibility and all-channel/alpha fallback", flush=True)

        # Preserve the memory1 reuse optimization for matching mask and RGBA
        # formats: an allocator failure must be irrelevant when no grow is due.
        for value in [bytes([128]) * 16, bytes([80, 40, 0, 120]) * 4]:
            content = qa.packet([qa.upload(1, 2, 2, value)], [quad])
            compare(content)
            calls = after.lib.qa_memory_calls()
            after.lib.qa_memory_fail_after(0)
            try:
                compare(content)
            finally:
                after.lib.qa_memory_fail_after(-1)
            assert after.lib.qa_memory_calls() == calls
        print("PASS matching-format full updates reuse storage without allocation", flush=True)

        colored = qa.rgba([(255, 0, 0, 255), (0, 255, 0, 255),
                           (0, 0, 255, 255), (255, 255, 255, 255)])
        for sets in [
            [qa.upload(1, 2, 2, mask([0, 64, 128, 255])),
             qa.upload(1, 1, 1, bytes([80, 40, 0, 100]), 1, 0),
             qa.upload(1, 2, 2, mask([255, 128, 64, 0]))],
            [qa.upload(1, 2, 2, colored), qa.upload(1, 2, 2, mask([1, 2, 3, 4])),
             qa.upload(1, 1, 1, bytes([16, 32, 48, 64]), 0, 1)],
            [qa.upload(1, 1, 1, mask([1])), qa.upload(1, 3, 1, mask([0, 128, 255])),
             qa.upload(1, 1, 1, mask([64]), 2, 0), qa.upload(1, 2, 2, colored)],
            [qa.upload(1, 2, 2, colored), qa.upload(1, 2, 2, mask([10, 20, 30, 40])),
             qa.upload(1, 1, 1, mask([120]), 1, 1)],
        ]:
            compare(qa.packet(sets, [quad]))
        print("PASS ordered full/partial/full transitions across size and format", flush=True)

        for iteration in range(150):
            coverage = mask([randomizer.randrange(256) for _ in range(64)])
            updates = [qa.upload(1, 8, 8, coverage)]
            if iteration % 3:
                patch = mask([randomizer.randrange(256) for _ in range(4)])
            else:
                patch = qa.rgba([(20, 40, 60, 100)] * 4)
            updates.append(qa.upload(1, 2, 2, patch, randomizer.randrange(7), randomizer.randrange(7)))
            vertices = []
            for _ in range(3 * randomizer.randrange(1, 15)):
                alpha = randomizer.randrange(256)
                color = (alpha << 24) | sum(randomizer.randrange(alpha + 1) << (8 * c) for c in range(3))
                vertices.append((randomizer.uniform(-10, 45), randomizer.uniform(-8, 28),
                                 randomizer.uniform(-.5, 1.5), randomizer.uniform(-.5, 1.5), color))
            compare(qa.packet(updates, [qa.mesh(1, vertices, list(range(len(vertices))), (-2., 1., 30., 18.))]))
        print("PASS 150 deterministic mixed-mask/color bilinear and blended triangle frames", flush=True)

        valid = qa.packet([qa.upload(1, 2, 2, mask([0, 80, 160, 255]))], [quad])
        compare(valid)
        bad_mesh = qa.mesh(1, [(float("nan"), 0., 0., 0., qa.MAX_U32)] * 3, [0, 1, 2])
        malformed = [qa.packet([qa.upload(1, 1, 1, bytes([20, 40, 60, 100]), 1, 1)], [bad_mesh]),
                     qa.packet([qa.upload(1, 1, 1, bytes([20, 40, 60, 100]), 2, 0)], [quad]),
                     qa.packet([qa.upload(1, 1, 1, bytes([20, 40, 60, 100]), 0, 0)], [qa.quad(999, 31, 17)]),
                     valid + b"x"]
        malformed += [valid[:n] for n in [0, 1, 4, 15, 16, 17, 35, 48, len(valid) - 1]]
        for data in malformed:
            texture_bytes = memory(after)["textures"]
            compare(data, expected=False)
            assert memory(after)["textures"] == texture_bytes
            compare(qa.packet([], [quad]))
        for _ in range(200):
            data = bytearray(valid)
            for _ in range(randomizer.randrange(1, 5)):
                data[randomizer.randrange(len(data))] ^= randomizer.randrange(1, 256)
            compare(bytes(data), expected=None)
        compare(qa.packet([], [], [1]))
        compare(qa.packet([], [quad]), expected=False)
        print("PASS malformed/parser mutation rollback and texture removal", flush=True)
    finally:
        before.close()
        after.close()

    failures = []
    # Allocation faults at promotion, size/format transitions, vertex scratch,
    # and the display surface must leave the previous image and cache valid.
    for scenario in ["promotion", "promotion-and-resize", "ordered-transitions", "full-recompaction"]:
        rejected = 0
        for position in range(12):
            renderer = qa.Renderer(libraries[1])
            try:
                initial = bytes([80, 40, 0, 120]) * 4 if scenario == "full-recompaction" else mask([0, 64, 128, 255])
                seed = qa.packet([qa.upload(1, 2, 2, initial)], [qa.quad(1, 23, 11)])
                assert renderer.frame(23, 11, seed)
                original = renderer.pixels()
                old_texture = memory(renderer)["textures"]
                if scenario == "promotion":
                    target = qa.packet([qa.upload(1, 1, 1, bytes([60, 30, 0, 100]), 1, 1)], [qa.quad(1, 23, 11)])
                    w, h = 23, 11
                elif scenario == "promotion-and-resize":
                    vertices = [(float(i % 21), float(i // 21), 0., 0., qa.MAX_U32) for i in range(630)]
                    target = qa.packet([qa.upload(1, 1, 1, bytes([60, 30, 0, 100]), 1, 1)],
                                       [qa.mesh(1, vertices, list(range(630)))])
                    w, h = 1025, 769
                elif scenario == "ordered-transitions":
                    target = qa.packet([qa.upload(1, 2, 2, mask([0, 64, 128, 255])),
                                        qa.upload(1, 1, 1, bytes([60, 30, 0, 100]), 1, 1),
                                        qa.upload(1, 3, 1, mask([64, 128, 255])),
                                        qa.upload(1, 1, 1, bytes([80, 40, 0, 120]), 2, 0)], [qa.quad(1, 23, 11)])
                    w, h = 23, 11
                else:
                    target = qa.packet([qa.upload(1, 2, 2, mask([0, 64, 128, 255]))],
                                       [qa.quad(1, 23, 11)])
                    w, h = 23, 11
                renderer.lib.qa_memory_fail_after(position)
                ok = renderer.frame(w, h, target)
                renderer.lib.qa_memory_fail_after(-1)
                if not ok:
                    rejected += 1
                    assert renderer.pixels() == original
                    assert memory(renderer)["textures"] == old_texture
                    assert renderer.frame(23, 11, qa.packet([], [qa.quad(1, 23, 11)]))
                    assert renderer.pixels() == original
            finally:
                renderer.close()
        assert rejected, scenario + " did not test a required allocation failure"
        failures.append({"scenario": scenario, "rejected_allocations": rejected})
    print("PASS required allocation failures and old mask/image rollback", flush=True)

    after.lib.qa_memory_fail_after(0)
    assert not after.lib.wc_raster_create(), "creation must fail cleanly when its metadata allocation fails"
    after.lib.qa_memory_fail_after(-1)
    assert after.lib.qa_memory_live() == 0

    sizes = []
    promotions = []
    for extent in [1024, 2048]:
        payload = qa.upload(5, extent, extent, bytes([128]) * (extent * extent * 4))
        old, new = [qa.Renderer(lib) for lib in libraries]
        try:
            assert old.frame(17, 13, qa.packet([payload], [qa.quad(5, 17, 13)]))
            assert new.frame(17, 13, qa.packet([payload], [qa.quad(5, 17, 13)]))
            assert old.pixels() == new.pixels()
            a, b = memory(old)["textures"], memory(new)["textures"]
            assert a == extent * extent * 4 and b == extent * extent
            sizes.append({"extent": extent, "baseline_cache_bytes": a,
                          "candidate_cache_bytes": b, "saved_bytes": a - b})
            patch = qa.packet([qa.upload(5, 1, 1, bytes([80, 40, 0, 100]), extent // 2, extent // 2)],
                              [qa.quad(5, 17, 13)])
            live = new.lib.qa_memory_live()
            calls = new.lib.qa_memory_calls()
            new.lib.qa_memory_reset_peak()
            assert old.frame(17, 13, patch) and new.frame(17, 13, patch)
            assert old.pixels() == new.pixels()
            peak = new.lib.qa_memory_peak() - live
            assert memory(new)["textures"] == a
            assert peak == a and new.lib.qa_memory_calls() - calls == 1
            assert old.frame(17, 13, qa.packet([payload], [qa.quad(5, 17, 13)]))
            assert new.frame(17, 13, qa.packet([payload], [qa.quad(5, 17, 13)]))
            assert old.pixels() == new.pixels() and memory(new)["textures"] == b
            promotions.append({"extent": extent, "mask_cache_before": b,
                               "RGBA_cache_after": a, "temporary_extra_bytes": peak,
                               "cache_after_full_recompaction": memory(new)["textures"]})
        finally:
            old.close()
            new.close()
    print("PASS measured 1024/2048-square coverage textures save 3/12 MiB", flush=True)

    report = {"scope": "Requested native-cache allocation bytes and exact renderer pixels; not OS/2 guest or RSS",
              "pixel_and_parser_frames": checks, "allocation_failures": failures,
              "creation_allocation_failure": "passed",
              "coverage_cache": sizes, "colored_partial_promotion": promotions,
              "passed": True}
    (build / "mask-results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2), flush=True)


if __name__ == "__main__":
    main()
