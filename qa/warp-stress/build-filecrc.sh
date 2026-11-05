#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "${1:-build/warp-stress}"
out=$(realpath "${1:-build/warp-stress}")
: "${WATCOM:?Set WATCOM}"
export INCLUDE="$WATCOM/h:$WATCOM/h/os2"
"$WATCOM/binl64/wcc386" -q -bt=os2 -3s -zp4 -mf -ot -zastd=c99 "-fo=$out/filecrc.obj" "$root/qa/warp-stress/filecrc.c"
cd "$out"
printf 'system os2v2\noption quiet\noption stack=131072\nname FILECRC.EXE\nfile filecrc.obj\nlibrary os2386\n' >filecrc.lnk
"$WATCOM/binl64/wlink" @filecrc.lnk
test -s FILECRC.EXE
