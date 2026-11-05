#!/usr/bin/env python3
"""Rebuild previous/current Grid PDF writers and compare exact bytes/heap peaks.

Needs the pinned grid-upstream checkout and Rust1.95. No OS/2 runtime implied.
Generated documents contain only contributor-generated geometry and text.
"""
from pathlib import Path
import argparse, hashlib, json, os, subprocess

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, default=root / 'build/export-memory/grid')
args = parser.parse_args()
build = args.build.resolve(); build.mkdir(parents=True, exist_ok=True)
baseline = build / 'baseline'; baseline.mkdir(exist_ok=True)
revision = '178f3eb745a589c63b61ef97dd7b6b2d6fd48515'
for name in ['lib.rs', 'metrics.rs', 'symbol.rs']:
    source = subprocess.check_output(['git', '-C', str(root / 'grid-upstream'), 'show', revision + ':crates/pdf/src/' + name])
    (baseline / name).write_bytes(source)
rustc = os.environ.get('RUSTC', 'rustc')
common = [rustc, '--edition', '2024', '-O']
subprocess.run([*common, '--crate-name', 'baseline', '--crate-type', 'rlib', str(baseline / 'lib.rs'), '-o', str(build / 'libbaseline.rlib')], check=True)
subprocess.run([*common, '--crate-name', 'candidate', '--crate-type', 'rlib', str(root / 'grid-upstream/crates/pdf/src/lib.rs'), '-o', str(build / 'libcandidate.rlib')], check=True)
probe = build / 'grid-pdf-probe'
subprocess.run([*common, str(root / 'qa/export-memory/grid-pdf-probe.rs'), '--extern', 'baseline=' + str(build / 'libbaseline.rlib'), '--extern', 'candidate=' + str(build / 'libcandidate.rlib'), '-o', str(probe)], check=True)
output = subprocess.check_output([str(probe), str(build / 'pdf')], text=True)
(build / 'grid-export.log').write_text(output)
print(output, end='')
results = []
for line in output.splitlines():
    if not line.startswith('PASS ') or 'baseline_peak=' not in line: continue
    fields = line.split(); item = {'fixture': fields[1]}
    item.update({key: int(value) for key, value in (field.split('=') for field in fields[2:])})
    path = build / 'pdf' / (item['fixture'] + '.pdf')
    item['sha256'] = hashlib.sha256(path.read_bytes()).hexdigest(); results.append(item)
(build / 'grid-export.json').write_text(json.dumps({'scope': 'native requested heap bytes; no OS/2 guest measurement', 'baseline_revision': revision, 'results': results, 'passed': len(results) == 3}, indent=2) + '\n')
