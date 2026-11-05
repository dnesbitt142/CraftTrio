#!/usr/bin/env python3
"""Run the actual OS/2 runtime branch with mmap/mprotect-backed API shims."""
from pathlib import Path
import argparse,os,subprocess
kit=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('build',nargs='?',type=Path,default=kit/'build/reclaim-pages')
parser.add_argument('--sanitize',action='store_true')
args=parser.parse_args()
build=args.build.resolve()
build.mkdir(parents=True,exist_ok=True)
source=kit/'qa/reclaim'
command=[os.environ.get('CC','gcc'),'-std=c99','-g','-D__OS2__','-DWASM_RT_USE_MMAP=0','-DWASM_RT_SKIP_SIGNAL_RECOVERY=1','-I',str(source/'os2-stub'),'-I',str(kit/'native/runtime'),str(source/'runtime-pages.c'),str(kit/'native/runtime/wasm-rt-impl.c'),str(kit/'native/runtime/wasm-rt-exceptions-impl.c'),'-lm','-o',str(build/'runtime-pages')]
if args.sanitize:command[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
subprocess.run(command,check=True)
environment={**os.environ,**({'ASAN_OPTIONS':'detect_leaks=0'} if args.sanitize else {})}
result=subprocess.run([str(build/'runtime-pages')],cwd=build,env=environment,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
(build/'runtime-pages.log').write_text(result.stdout)
print(result.stdout,end='')
result.check_returncode()
