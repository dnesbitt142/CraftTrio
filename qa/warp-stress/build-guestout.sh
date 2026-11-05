#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
out=$(realpath "${1:-build/warp-stress}")
mkdir -p "$out"
: "${WATCOM:?Set WATCOM}"
"$WATCOM/binl64/wcc386" -q -bt=os2 -3s -zp4 -mf -ot -zastd=c99 "-fo=$out/guestout.obj" "$root/qa/warp-stress/guestout.c"
cd "$out"
printf 'system os2v2\noption quiet\nname GUESTOUT.EXE\nfile guestout.obj\nlibrary os2386\n' > guestout.lnk
"$WATCOM/binl64/wlink" @guestout.lnk
test -s GUESTOUT.EXE
