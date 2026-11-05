#!/usr/bin/env python3
"""Compare the new renderer against the previous shared-host renderer.

Checks canonical pixels and parser/rollback behavior, and accounts requested
allocation bytes. This is a build-host test, not an OS/2 GPI or RSS measurement.
"""
import argparse
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import random
import struct
import subprocess

KIT = Path(__file__).resolve().parents[2]
QA = Path(__file__).resolve().parent
MAX_U32 = 0xffffffff


def upload(texture, width, height, pixels, x=MAX_U32, y=MAX_U32):
    assert len(pixels) == width * height * 4
    return struct.pack('<5I', texture, x, y, width, height) + pixels


def rgba(colors):
    return b''.join(bytes(color) for color in colors)


def mesh(texture, vertices, indices, clip=(0., 0., 8192., 8192.)):
    return (struct.pack('<I4f2I', texture, *clip, len(vertices), len(indices)) +
            b''.join(struct.pack('<4fI', *v) for v in vertices) +
            struct.pack('<' + 'I' * len(indices), *indices))


def quad(texture, width, height, color=MAX_U32, clip=None):
    vertices = [(0., 0., 0., 0., color), (float(width), 0., 1., 0., color),
                (float(width), float(height), 1., 1., color),
                (0., float(height), 0., 1., color)]
    return mesh(texture, vertices, [0, 1, 2, 0, 2, 3], clip or (0., 0., float(width), float(height)))


def packet(uploads=(), meshes=(), frees=()):
    return (b'WCG1' + struct.pack('<3I', len(uploads), len(meshes), len(frees)) +
            b''.join(uploads) + b''.join(meshes) + struct.pack('<'+'I'*len(frees), *frees))


class Renderer:
    def __init__(self, library, baseline=False):
        self.lib = C.CDLL(str(library))
        self.baseline = baseline
        for name in ['qa_memory_live', 'qa_memory_peak', 'qa_memory_calls']:
            getattr(self.lib, name).restype = C.c_size_t
        self.lib.qa_memory_fail_after.argtypes = [C.c_long]
        self.lib.wc_raster_create.restype = C.c_void_p
        self.lib.wc_raster_destroy.argtypes = [C.c_void_p]
        self.lib.wc_raster_frame.argtypes = [C.c_void_p, C.c_uint32, C.c_uint32, C.c_void_p, C.c_size_t]
        self.lib.wc_raster_frame.restype = C.c_int
        self.lib.wc_raster_pixels.argtypes = [C.c_void_p, C.POINTER(C.c_uint32), C.POINTER(C.c_uint32), C.POINTER(C.c_size_t)]
        self.lib.wc_raster_pixels.restype = C.c_void_p
        self.lib.wc_raster_error.argtypes = [C.c_void_p]
        self.lib.wc_raster_error.restype = C.c_char_p
        self.instance = self.lib.wc_raster_create()
        assert self.instance

    def close(self):
        if self.instance:
            self.lib.qa_memory_fail_after(-1)
            self.lib.wc_raster_destroy(self.instance)
            self.instance = None
            assert self.lib.qa_memory_live() == 0, 'renderer allocations leaked'

    def frame(self, width, height, data):
        buffer = C.create_string_buffer(data)
        return bool(self.lib.wc_raster_frame(self.instance, width, height, buffer, len(data)))

    def pixels(self):
        width, height, stride = C.c_uint32(), C.c_uint32(), C.c_size_t()
        ptr = self.lib.wc_raster_pixels(self.instance, C.byref(width), C.byref(height), C.byref(stride))
        if not ptr or not width.value or not height.value:
            return (width.value, height.value, b'')
        raw = C.string_at(ptr, stride.value * height.value)
        output = bytearray()
        if self.baseline:
            assert stride.value == width.value * 4
            for y in range(height.value):
                row = raw[y*stride.value:(y+1)*stride.value]
                for x in range(width.value):
                    assert row[4*x+3] == 255
                    output.extend(row[4*x:4*x+3])
        else:
            assert stride.value == (width.value*3+3)&~3
            for y in range(height.value-1, -1, -1):
                row = raw[y*stride.value:(y+1)*stride.value]
                assert not any(row[width.value*3:]), 'nonzero bitmap padding'
                output.extend(row[:width.value*3])
        return width.value, height.value, bytes(output)

    def error(self):
        return self.lib.wc_raster_error(self.instance).decode()


def build_library(host, build, name, baseline):
    build.mkdir(parents=True, exist_ok=True)
    cc = os.environ.get('CC', 'gcc')
    flags = [cc, '-std=c99', '-O0', '-g', '-fPIC', '-I', str(host)]
    sources = [host/'raster.c'] + ([] if baseline else [host/'surface.c'])
    objects = []
    for source in sources:
        obj = build/(name+'-'+source.stem+'.o')
        subprocess.run([*flags, '-include', str(QA/'alloc-shim.h'), '-c', str(source), '-o', str(obj)], check=True)
        objects.append(obj)
    alloc = build/(name+'-alloc.o')
    subprocess.run([*flags, '-c', str(QA/'alloc-shim.c'), '-o', str(alloc)], check=True)
    output = build/(name+'.so')
    subprocess.run([cc, '-shared', '-Wl,-Bsymbolic', *map(str, objects), str(alloc), '-o', str(output)], check=True)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True, help='previous native/host directory')
    parser.add_argument('--build', type=Path, default=KIT.parent/'build/memory-renderer')
    args = parser.parse_args()
    build = args.build.resolve()
    baseline_lib = build_library(args.baseline.resolve(), build, 'baseline', True)
    candidate_lib = build_library(KIT/'native/host', build, 'candidate', False)
    old, new = Renderer(baseline_lib, True), Renderer(candidate_lib)
    assertions = 0
    randomizer = random.Random(0x4352414654)

    def compare(width, height, data, expected=None):
        nonlocal assertions
        before = old.pixels(), new.pixels()
        a, b = old.frame(width, height, data), new.frame(width, height, data)
        assert a == b, ('status mismatch', a, b, old.error(), new.error())
        if expected is not None:
            assert a == expected, (old.error(), new.error())
        if a:
            assert old.pixels() == new.pixels(), 'pixel mismatch'
        else:
            assert old.pixels() == before[0], 'baseline invalid-frame image changed'
            assert new.pixels() == before[1], 'candidate invalid-frame image changed'
        assertions += 1
        return a

    seed = upload(1, 1, 1, bytes([255]*4))
    try:
        # The expected BGR texels are independent of the reference renderer.
        colored = rgba([(255,0,0,255), (0,255,0,255), (0,0,255,255), (255,255,255,255)])
        compare(2, 2, packet([upload(2,2,2,colored)], [quad(2,2,2)]), True)
        assert new.pixels()[2] == bytes([0,0,255, 0,255,0, 255,0,0, 255,255,255])
        for width in [1,3,5,13,31,64]:
            for height in [1,2,7,19]:
                compare(width,height,packet([seed],[quad(1,width,height,0xff2478c9)]),True)
        print('PASS golden colors, vertical orientation, odd widths and row padding', flush=True)

        for _ in range(150):
            width, height = randomizer.randrange(1,70), randomizer.randrange(1,60)
            vertices=[]
            for _ in range(3*randomizer.randrange(1,15)):
                a=randomizer.randrange(256)
                color=(a<<24)|sum(randomizer.randrange(a+1)<<(8*c) for c in range(3))
                vertices.append((randomizer.uniform(-20,width+20),randomizer.uniform(-20,height+20),
                                 randomizer.uniform(-.5,1.5),randomizer.uniform(-.5,1.5),color))
            clip=(randomizer.uniform(-10,width/2),randomizer.uniform(-10,height/2),
                  randomizer.uniform(width/2,width+10),randomizer.uniform(height/2,height+10))
            compare(width,height,packet([], [mesh(2,vertices,list(range(len(vertices))),clip)]),True)
        print('PASS 150 deterministic randomized textured, blended, clipped triangle frames', flush=True)

        for sets in [
            [upload(3,2,2,colored),upload(3,2,2,colored[::-1])],
            [upload(3,2,2,colored),upload(3,1,1,bytes([64,0,0,64]),1,1)],
            [upload(3,3,1,rgba([(30,20,10,255)]*3)),upload(3,1,1,bytes([1,2,3,255]),2,0),upload(3,2,2,colored)],
            [upload(3,2,2,colored),upload(3,1,1,bytes([255,255,255,255]),0,0),upload(3,2,2,colored)],
        ]:
            compare(27,17,packet(sets,[quad(3,27,17)]),True)
        print('PASS duplicate texture IDs and full/partial/full update sequences',flush=True)

        valid=packet([upload(3,2,2,colored)],[quad(3,23,11)])
        compare(23,11,valid,True)
        bad_mesh=mesh(3,[(float('nan'),0.,0.,0.,MAX_U32)]*3,[0,1,2])
        malformed=[packet([upload(3,2,2,bytes(16))],[bad_mesh]),
                   packet([upload(3,2,2,bytes(16))],[quad(999,23,11)]),
                   packet([upload(3,1,1,bytes(4),5,0)],[]),valid+b'x',b'BAD!'+valid[4:]]
        malformed.extend(valid[:n] for n in sorted(set([0,1,4,15,16,17,35,50,len(valid)-1])))
        for data in malformed:
            compare(23,11,data,False)
            compare(23,11,packet([],[quad(3,23,11)]),True)
        print('PASS invalid upload/mesh/NaN/truncation rollback retains pixels and texture content',flush=True)

        for _ in range(200):
            data=bytearray(valid)
            for _ in range(randomizer.randrange(1,5)):
                position=randomizer.randrange(len(data));data[position]^=randomizer.randrange(1,256)
            compare(23,11,bytes(data))
        compare(23,11,packet([],[],[3]),True)
        compare(23,11,packet([],[quad(3,23,11)]),False)
        print('PASS 200 deterministic packet mutations and texture removal',flush=True)
    finally:
        old.close();new.close()

    # Candidate transactional behavior under failed required allocations.
    required_failures=0
    for failure in range(8):
        candidate=Renderer(candidate_lib)
        try:
            assert candidate.frame(23,11,packet([seed],[quad(1,23,11)]))
            before=candidate.pixels()
            candidate.lib.qa_memory_fail_after(failure)
            complex_vertices=[(float(i%21),float(i//21),0.,0.,MAX_U32) for i in range(630)]
            next_frame=packet([upload(1,8,8,bytes([128,0,0,128])*64)],
                              [mesh(1,complex_vertices,list(range(630)))])
            success=candidate.frame(1025,769,next_frame)
            candidate.lib.qa_memory_fail_after(-1)
            if not success:
                required_failures+=1
                assert candidate.pixels()==before
                assert candidate.frame(23,11,packet([],[quad(1,23,11)]))
                assert candidate.pixels()==before
        finally:candidate.close()
    assert required_failures
    print('PASS allocation-failure rollback and cleanup',flush=True)

    candidate=Renderer(candidate_lib)
    try:
        assert candidate.frame(1024,1024,packet())
        live=candidate.lib.qa_memory_live()
        candidate.lib.qa_memory_fail_after(0)
        assert candidate.frame(31,17,packet()),'optional trim allocation must not reject a valid frame'
        assert candidate.lib.qa_memory_live()==live
        assert candidate.pixels()==(31,17,bytes([240])*(31*17*3))
        candidate.lib.qa_memory_fail_after(-1)
        assert candidate.frame(31,17,packet())
        assert candidate.lib.qa_memory_live()<live
    finally:candidate.close()
    print('PASS failed optional surface trim retains a valid new image and later trim succeeds',flush=True)

    measurements=[]
    for width,height in [(1280,751),(1920,1080),(2560,1600)]:
        before,after=Renderer(baseline_lib,True),Renderer(candidate_lib)
        try:
            old_base=before.lib.qa_memory_live();new_base=after.lib.qa_memory_live()
            assert before.frame(width,height,packet()) and after.frame(width,height,packet())
            old_surface=before.lib.qa_memory_live()-old_base
            new_surface=after.lib.qa_memory_live()-new_base
            old_pm=((width*3+3)&~3)*height
            assert old_surface==4*width*height
            assert new_surface==old_pm
            measurements.append({'width':width,'height':height,'baseline_raster_bytes':old_surface,
                                 'baseline_PM_copy_bytes':old_pm,'candidate_surface_bytes':new_surface,
                                 'surface_buffers_saved_bytes':old_surface+old_pm-new_surface})
        finally:before.close();after.close()

    before,after=Renderer(baseline_lib,True),Renderer(candidate_lib)
    try:
        for renderer in [before,after]:assert renderer.frame(2560,1600,packet())
        peak_old=before.lib.qa_memory_live();peak_new=after.lib.qa_memory_live()
        for renderer in [before,after]:assert renderer.frame(640,480,packet())
        shrink={'baseline_live_before':peak_old,'baseline_live_after':before.lib.qa_memory_live(),
                'candidate_live_before':peak_new,'candidate_live_after':after.lib.qa_memory_live()}
        assert shrink['candidate_live_after']<shrink['candidate_live_before']
        assert shrink['baseline_live_before']==shrink['baseline_live_after']
    finally:before.close();after.close()

    vertex_shrink=[]
    for baseline,library in [(True,baseline_lib),(False,candidate_lib)]:
        renderer=Renderer(library,baseline)
        try:
            vertices=[(0.,0.,0.,0.,MAX_U32)]*20001
            assert renderer.frame(9,7,packet([],[mesh(0,vertices,[])]))
            live=renderer.lib.qa_memory_live()
            assert renderer.frame(9,7,packet())
            vertex_shrink.append({'baseline':baseline,'live_before':live,'live_after':renderer.lib.qa_memory_live()})
        finally:renderer.close()
    assert vertex_shrink[0]['live_after']==vertex_shrink[0]['live_before']
    assert vertex_shrink[1]['live_after']<vertex_shrink[1]['live_before']

    texture=[]
    for baseline,library in [(True,baseline_lib),(False,candidate_lib)]:
        renderer=Renderer(library,baseline)
        try:
            data=upload(10,512,512,bytes([255]*4)*(512*512))
            assert renderer.frame(7,5,packet([data]))
            renderer.lib.qa_memory_reset_peak()
            live=renderer.lib.qa_memory_live();calls=renderer.lib.qa_memory_calls()
            assert renderer.frame(7,5,packet([data]))
            texture.append({'baseline':baseline,'live_before':live,'peak_extra_bytes':renderer.lib.qa_memory_peak()-live,
                            'allocation_calls':renderer.lib.qa_memory_calls()-calls})
        finally:renderer.close()
    report={'scope':'C allocation bytes and canonical pixels; not OS/2 guest, GPI, or process RSS',
            'pixel_and_parser_frames':assertions,'required_allocation_failures_checked':required_failures,
            'surfaces':measurements,'shrink':shrink,'vertex_shrink':vertex_shrink,
            'same_size_texture_replacement':texture,'passed':True}
    (build/'renderer-results.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2),flush=True)
    return 0


if __name__=='__main__':raise SystemExit(main())
