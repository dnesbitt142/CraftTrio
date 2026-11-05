#!/usr/bin/env bash
set -euo pipefail
suite=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "${1:?Pass a build directory}"
build=$(realpath "$1")
: "${WATCOM:?Set WATCOM to OpenWatcom v2}"
export PATH="$WATCOM/binl64:$PATH"
export INCLUDE="$WATCOM/h:$WATCOM/h/os2"
flags=(-q -bt=os2 -3s -zp4 -mf -zastd=c99 -ot
  -dWASM_RT_USE_MMAP=0 -dWASM_RT_SKIP_SIGNAL_RECOVERY=1
  "-i=$suite/native/runtime" "-fi=$suite/native/host/owcompat.h")
for name in wasm-rt-impl wasm-rt-exceptions-impl; do
  "$WATCOM/binl64/wcc386" "${flags[@]}" "-fo=$build/$name.obj" "$suite/native/runtime/$name.c"
done
"$WATCOM/binl64/wcc386" "${flags[@]}" "-fo=$build/probe.obj" "$suite/qa/reclaim/native-pages.c"
{
  printf 'system os2v2\noption quiet\noption stack=1048576\nname %s/RECLTEST.EXE\n' "$build"
  for name in probe wasm-rt-impl wasm-rt-exceptions-impl; do printf 'file %s/%s.obj\n' "$build" "$name"; done
} > "$build/probe.lnk"
"$WATCOM/binl64/wlink" "@$build/probe.lnk"
printf '%s\n' "$build/RECLTEST.EXE"
