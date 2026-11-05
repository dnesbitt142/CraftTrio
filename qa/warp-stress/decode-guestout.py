#!/usr/bin/env python3
"""Decode validated complete guest-output records without trusting guest paths."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import re

def decode(data, destination):
    destination=Path(destination);destination.mkdir(parents=True,exist_ok=True)
    results=[]; pending=None; chunks=[]
    for raw in data.splitlines():
        line=raw.strip()
        if line.startswith(b'GUESTOUT-BEGIN '):
            if pending is not None:raise ValueError('Incomplete record before next header')
            _,size,hpath=line.split()
            size=int(size)
            if not 0<=size<=67108864:raise ValueError('Record size out of bounds')
            path=bytes.fromhex(hpath.decode()).decode('ascii')
            pending=(size,path);chunks=[]
        elif line.startswith(b'GUESTOUT-END '):
            if pending is None:raise ValueError('End without begin')
            content=base64.b64decode(b''.join(chunks),validate=True)
            size,path=pending
            if len(content)!=size or int(line.split()[1])!=size:raise ValueError('Size mismatch')
            safe=re.sub('[^A-Za-z0-9_.-]', '_', path)
            name=f'{len(results):03d}-'+safe
            target=destination/name
            if target.exists():raise FileExistsError(target)
            target.write_bytes(content)
            results.append({'guest_path':path,'path':str(target),'bytes':size,'sha256':hashlib.sha256(content).hexdigest()})
            pending=None;chunks=[]
        elif line.startswith(b'GUESTOUT-ERROR '):
            fields=line.split();results.append({'guest_path':bytes.fromhex(fields[1].decode()).decode('ascii'),'error':int(fields[2])})
        elif line==b'GUESTOUT-ABORT':raise ValueError('Guest aborted a file')
        elif pending is not None:
            if len(line)>64:raise ValueError('Oversized base64 line')
            chunks.append(line)
    if pending is not None:raise ValueError('Truncated record')
    return results

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('serial_log');p.add_argument('destination');a=p.parse_args()
    print(json.dumps(decode(Path(a.serial_log).read_bytes(),a.destination),indent=2))
