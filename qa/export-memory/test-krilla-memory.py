#!/usr/bin/env python3
"""Measure actual old/new Krilla split functions without an application GUI.

Run the vendor Krilla native image tests first to build flate2, then source
this kit's toolchain environment and pass --dependencies /target/debug/deps.
The source functions are extracted unchanged from the pinned baseline/current
backend. Only their surrounding private enum types are stubbed for this probe.
"""
from pathlib import Path
import argparse, json, os, subprocess
root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--dependencies', type=Path, required=True)
parser.add_argument('--build', type=Path, default=root / 'build/export-memory/krilla')
args = parser.parse_args(); build = args.build.resolve(); build.mkdir(parents=True, exist_ok=True)

def function(source, name):
    start = source.index('fn ' + name + '(')
    opening = source.index('{', start); nesting = 1; end = opening + 1
    while nesting:
        if source[end] == '{': nesting += 1
        elif source[end] == '}': nesting -= 1
        end += 1
    return source[start:end]

old = subprocess.check_output(['git', '-C', str(root / 'gui-craft-libs'), 'show', '92d392cff132eaf53e13afff77f38b5c11b73aa0:vendor/krilla/src/graphics/image.rs'], text=True)
new = (root / 'gui-craft-libs/vendor/krilla/src/graphics/image.rs').read_text()
stream = (root / 'gui-craft-libs/vendor/krilla/src/stream.rs').read_text()
tracking = (root / 'qa/export-memory/grid-pdf-probe.rs').read_text().split('macro_rules! document')[0].replace('use std::path::Path;\n', '')
source = tracking + '\nuse flate2::{write::ZlibEncoder, Compression};\n'
source += function(stream, 'deflate_encoder') + '\n' + function(stream, 'deflate_encode') + '\n'
source += '''\n#[allow(dead_code, unreachable_patterns)] mod baseline {
use super::*;
enum ColorSpace { RGB, RGBA, Luma, LumaA }
impl ColorSpace {
    fn has_alpha(&self) -> bool { matches!(self, Self::RGBA | Self::LumaA) }
    fn num_components(&self) -> usize { match self { Self::RGB => 3, Self::RGBA => 4, Self::Luma => 1, Self::LumaA => 2 } }
}
enum BitsPerComponent { Eight }
type SplitImage = (Vec<u8>, Option<Vec<u8>>, BitsPerComponent);
'''
source += function(old, 'try_buffer') + '\n' + function(old, 'handle_u8_image')
source += '\npub fn run(data: &[u8]) -> (Vec<u8>,Option<Vec<u8>>) { let(c,a,_)=handle_u8_image(data,ColorSpace::RGBA).unwrap(); (c,a) }\n}\n'
source += 'mod candidate { use super::*;\n' + function(new,'stream_image_channels')
source += '\npub fn run(data: &[u8]) -> (Vec<u8>,Option<Vec<u8>>) { stream_image_channels(data,3,1).unwrap() }\n}\n'
source += r'''
fn measured(run:impl FnOnce()->(Vec<u8>,Option<Vec<u8>>))->((Vec<u8>,Option<Vec<u8>>),usize){
    let before=LIVE.load(Ordering::SeqCst); PEAK.store(before,Ordering::SeqCst);
    let output=black_box(run()); let peak=PEAK.load(Ordering::SeqCst)-before; (output,peak)
}
fn main(){
    println!("Krilla channel-split allocation probe; requested bytes, not OS/2 process RSS");
    for varied in [false,true] {
        let mut data=vec![255u8;1024*1024*4];
        if varied { let mut s=0x12345678u32; for p in data.chunks_exact_mut(4) {
            s^=s<<13;s^=s>>17;s^=s<<5;p.copy_from_slice(&s.to_le_bytes());
        }}
        let(old,old_peak)=measured(||baseline::run(&data));
        let(new,new_peak)=measured(||candidate::run(&data));
        assert_eq!(new,old,"compressed image bytes must remain identical");
        assert!(new_peak<old_peak,"streaming did not lower requested allocation peak");
        println!("PASS {} input_bytes={} baseline_peak={} candidate_peak={} saved_peak={}",
            if varied {"varied-rgba"} else {"opaque-flat"},data.len(),old_peak,new_peak,old_peak-new_peak);
    }
}
'''
probe_source=build/'krilla-probe.rs';probe_source.write_text(source)
rlibs=sorted(args.dependencies.resolve().glob('libflate2-*.rlib'),key=lambda p:p.stat().st_mtime,reverse=True)
if not rlibs:raise SystemExit('No native libflate2 .rlib in --dependencies directory')
exe=build/'krilla-probe'
subprocess.run([os.environ.get('RUSTC','rustc'),'--edition','2024','-O',str(probe_source),'--extern','flate2='+str(rlibs[0]),'-L','dependency='+str(args.dependencies.resolve()),'-o',str(exe)],check=True)
output=subprocess.check_output([str(exe)],text=True);print(output,end='');(build/'krilla-memory.log').write_text(output)
results=[]
for line in output.splitlines():
    if line.startswith('PASS '):
        fields=line.split();item={'fixture':fields[1]};item.update({k:int(v) for k,v in (s.split('=') for s in fields[2:])});results.append(item)
(build/'krilla-memory.json').write_text(json.dumps({'scope':'native requested heap bytes, not OS/2 guest RSS','results':results,'passed':len(results)==2},indent=2)+'\n')
