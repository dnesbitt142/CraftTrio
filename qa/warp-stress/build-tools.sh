#!/usr/bin/env bash
set -euo pipefail
source_root=$(cd "$(dirname "$0")" && pwd)
mkdir -p "${1:?Pass a build directory}"
build=$(realpath "$1")
: "${WATCOM:?Source toolchain/env.sh first}"
export PATH="$WATCOM/binl64:$PATH"
export INCLUDE="$WATCOM/h:$WATCOM/h/os2"
for name in pressure memwatch; do
  "$WATCOM/binl64/wcc386" -q -bt=os2 -3s -zp4 -mf -zastd=c99 -ot -w4 \
    "-fo=$build/$name.obj" "$source_root/$name.c"
  upper=${name^^}
  {
    printf 'system os2v2\noption quiet\noption stack=65536\nname %s/%s.EXE\n' "$build" "$upper"
    printf 'file %s/%s.obj\n' "$build" "$name"
  } > "$build/$name.lnk"
  "$WATCOM/binl64/wlink" "@$build/$name.lnk"
done
printf '%s\n' "$build/PRESSURE.EXE" "$build/MEMWATCH.EXE"
