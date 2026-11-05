#!/usr/bin/env bash
set -euo pipefail
source_root=$(cd "$(dirname "$0")" && pwd)
mkdir -p "${1:?Pass a build directory}"
build=$(realpath "$1")
: "${WATCOM:?Source toolchain/env.sh first}"
export INCLUDE="$WATCOM/h:$WATCOM/h/os2"
"$WATCOM/binl64/wcc386" -q -bt=os2 -3s -zp4 -mf -zastd=c99 -ot -w4 \
  "-fo=$build/runtrio.obj" "$source_root/runtrio.c"
{
  printf 'system os2v2\noption quiet\noption stack=65536\nname %s/RUNTRIO.EXE\n' "$build"
  printf 'file %s/runtrio.obj\n' "$build"
} > "$build/runtrio.lnk"
"$WATCOM/binl64/wlink" "@$build/runtrio.lnk"
printf '%s\n' "$build/RUNTRIO.EXE"
