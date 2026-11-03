#!/usr/bin/env python3
"""Bind a native application icon to an LX executable with OpenWatcom WRC."""

import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile


def has_icon(path, size):
    with path.open('rb') as file:
        if file.read(2) != b'MZ':
            return False
        file.seek(0x3C)
        lx = struct.unpack('<I', file.read(4))[0]
        file.seek(lx)
        if file.read(2) != b'LX':
            return False
        file.seek(lx + 0x50)
        offset, count = struct.unpack('<II', file.read(8))
        file.seek(lx + offset)
        for _ in range(count):
            kind, number, length, _, _ = struct.unpack('<HHIHI', file.read(14))
            if (kind, number, length) == (1, 1, size):
                return True
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('app', choices=('WORDCRFT', 'DECKCRFT', 'GRIDCRFT'))
    parser.add_argument('executable', type=Path)
    parser.add_argument('--wrc', default=os.environ.get('CRAFT_WRC'),
                        help='resource compiler path or command')
    args = parser.parse_args()

    compiler = args.wrc
    if compiler is None:
        watcom = os.environ.get('WATCOM')
        compiler = str(Path(watcom) / 'binl64/wrc') if watcom else 'wrc'
    found = shutil.which(compiler)
    if found is None:
        parser.error(f'OpenWatcom WRC not found at {compiler}')
    wrc = str(Path(found).resolve())
    executable = args.executable.resolve(strict=True)
    icon = Path(__file__).resolve().with_name(f'{args.app}.ICO')

    with tempfile.TemporaryDirectory(prefix='.craft-icon-', dir=executable.parent) as directory:
        work = Path(directory)
        shutil.copyfile(icon, work / icon.name)
        (work / 'icon.rc').write_text(f'ICON 1 "{icon.name}"\n', encoding='ascii')
        command = [wrc, '-q', '-bt=os2']
        subprocess.run(command + ['-r', '-fo=icon.res', 'icon.rc'], cwd=work, check=True)
        # WRC reconstructs a command line from argv and parses the quotes itself.
        subprocess.run(command + ['-fe=bound.exe', 'icon.res', f'"{executable}"'], cwd=work, check=True)
        bound = work / 'bound.exe'
        if not has_icon(bound, icon.stat().st_size):
            raise SystemExit('WRC output lacks the native application icon.')
        shutil.copymode(executable, bound)
        bound.replace(executable)


if __name__ == '__main__':
    main()
