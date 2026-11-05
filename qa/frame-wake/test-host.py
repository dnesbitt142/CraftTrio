#!/usr/bin/env python3
"""Compile actual candidate scheduling and render functions with controlled APIs."""
import argparse,hashlib,json,os,re,shlex,subprocess
from pathlib import Path
root=Path(__file__).resolve().parent
suite=root.parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source',type=Path,default=suite/'native/host/pm.c')
parser.add_argument('--output',type=Path,default=suite/'build/frame-wake')
args=parser.parse_args()
source=args.source.resolve()
out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
s=source.read_text()
def function(name):
 m=re.search(r'^static [^\n]*\b'+name+r'\([^;]*?\)\s*\{',s,re.M)
 if not m:raise ValueError(name)
 pos=s.index('{',m.start())+1;depth=1
 while depth:
  if s[pos]=='{':depth+=1
  elif s[pos]=='}':depth-=1
  pos+=1
 return s[m.start():pos]+'\n'
begin=s.index('static struct App {');end=s.index('} app;',begin)+len('} app;')
defines='\n'.join(re.findall(r'^#define (?:POINTER_TIMER|FRAME_TIMER|FRAME_WAKE_MS|MAX_VIEW_WIDTH|MAX_VIEW_HEIGHT|MAX_DRAW_PACKET) .*$',s,re.M))
(out/'app.inc').write_text(defines+'\n'+s[begin:end]+'\n')
for name,file in [('cancel_frame_wake','cancel'),('fail','fail'),('schedule_frame_wake','schedule'),('request_frame','request'),('invoke','invoke'),('render_frame_contents','contents'),('render_frame','render'),('frame_wake','wake')]:
 (out/(file+'.inc')).write_text(function(name))
dispatch=function('client_proc');a=dispatch.index('    case WM_TIMER:');b=dispatch.index('    case WM_CLOSE:',a)
(out/'dispatch.inc').write_text('static void dispatch(HWND window,ULONG msg,MPARAM mp1) {\n switch(msg) {\n'+dispatch[a:b].replace('        return 0;','        return;')+' }\n}\n')
assert 'done:\n    app.ready = 0;\n    cancel_frame_wake();' in s
exe=out/'probe'
cmd=shlex.split(os.getenv('CC','cc'))+['-std=c99','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I'+str(out),'-I'+str(suite/'native/host'),str(root/'host-regression.c'),'-o',str(exe)]
subprocess.run(cmd,check=True,timeout=60)
p=subprocess.run([str(exe)],text=True,capture_output=True,timeout=30)
passed=p.returncode==0 and p.stdout.count('PASS ')==8 and 'runtime error:' not in p.stderr and 'ERROR: AddressSanitizer' not in p.stderr
report={'scope':'extracted candidate PM timer/render state under host shims; no native timer latency or document benchmark','candidateSha256':hashlib.sha256(source.read_bytes()).hexdigest(),'passed':passed,'returncode':p.returncode,'stdout':p.stdout,'stderr':p.stderr}
(out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print(p.stdout,end='');print(p.stderr,end='');raise SystemExit(0 if passed else 1)
