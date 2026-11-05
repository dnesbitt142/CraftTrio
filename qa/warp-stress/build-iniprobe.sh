#!/usr/bin/env bash
set -euo pipefail
suite=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "${1:?Pass output directory}"
build=$(realpath "$1")
: "${WATCOM:?Set WATCOM to OpenWatcom}"
export PATH="$WATCOM/binl64:$PATH"
export INCLUDE="$WATCOM/h:$WATCOM/h/os2"
"$WATCOM/binl64/wcc386" -q -bt=os2 -3s -zp4 -mf -zastd=c99 -ot \
  "-fo=$build/iniprobe.obj" "$suite/qa/warp-stress/iniprobe.c"
{
  printf 'system os2v2\noption quiet\noption stack=131072\nname %s/INIPROBE.EXE\nfile %s/iniprobe.obj\n' "$build" "$build"
} > "$build/iniprobe.lnk"
"$WATCOM/binl64/wlink" "@$build/iniprobe.lnk"
printf '%s\n' "$build/INIPROBE.EXE"
