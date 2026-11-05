#!/usr/bin/env bash
set -euo pipefail
source_root=$(cd "$(dirname "$0")" && pwd)
mkdir -p "${1:?Pass a build directory}"
build=$(realpath "$1")
: "${WATCOM:?Source toolchain/env.sh first}"
export INCLUDE="$WATCOM/h:$WATCOM/h/os2"
"$WATCOM/binl64/wcc386" -q -bt=os2 -3s -zp4 -mf -zastd=c99 -ot -w4 \
  "-fo=$build/seragent.obj" "$source_root/seragent.c"
{
  printf 'system os2v2\noption quiet\noption stack=65536\nname %s/SERAGENT.EXE\n' "$build"
  printf 'file %s/seragent.obj\n' "$build"
} > "$build/seragent.lnk"
"$WATCOM/binl64/wlink" "@$build/seragent.lnk"
printf '%s\n' "$build/SERAGENT.EXE"
