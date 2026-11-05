#!/usr/bin/env bash
# Build an actual OS/2 contract probe; executing it in the guest is separate.
set -euo pipefail
suite=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "${1:?Pass an output directory}"
build=$(realpath "$1")
runtime=$(realpath "${2:-$suite/native/runtime/wasm-rt-impl.c}")
: "${WATCOM:?Set the OpenWatcom toolchain directory}"
export INCLUDE="$WATCOM/h:$WATCOM/h/os2"
flags=(-q -bt=os2 -3s -zp4 -mf -zastd=c99 -ot
       -dWASM_RT_USE_MMAP=0 -dWASM_RT_SKIP_SIGNAL_RECOVERY=1
       "-i=$suite/native/runtime" "-fi=$suite/native/host/owcompat.h")
"$WATCOM/binl64/wcc386" "${flags[@]}" "-fo=$build/runtime.obj" "$runtime"
"$WATCOM/binl64/wcc386" "${flags[@]}" "-fo=$build/exceptions.obj" \
    "$suite/native/runtime/wasm-rt-exceptions-impl.c"
"$WATCOM/binl64/wcc386" "${flags[@]}" "-fo=$build/probe.obj" \
    "$suite/qa/paging/native-fresh.c"
{
    printf 'system os2v2\noption quiet\noption stack=1048576\nname %s/PGFRESH.EXE\n' "$build"
    for name in probe runtime exceptions; do printf 'file %s/%s.obj\n' "$build" "$name"; done
} > "$build/probe.lnk"
"$WATCOM/binl64/wlink" "@$build/probe.lnk"
printf '%s\n' "$build/PGFRESH.EXE"
