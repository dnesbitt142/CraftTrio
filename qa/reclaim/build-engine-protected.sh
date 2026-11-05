#!/usr/bin/env bash
set -euo pipefail
suite=$(cd "$(dirname "$0")/../.." && pwd)
engine=$(realpath "${1:?Pass existing engine object directory}")
mkdir -p "${2:?Pass separate QA build directory}"
build=$(realpath "$2")
: "${WATCOM:?Source toolchain/env.sh}"
export INCLUDE="$WATCOM/h"
flags=(-q -bt=linux -3s -zp4 -mf -zastd=c99 -ot -d_SSIZE_T_DEFINED -d__OBSCURE_STREAM_INTERNALS
  -dWASM_RT_USE_MMAP=0 -dWASM_RT_SKIP_SIGNAL_RECOVERY=1
  "-i=$suite/native/host" "-i=$suite/native/runtime" "-i=$engine/engine"
  "-i=$suite/qa/reclaim/os2-stub" "-fi=$suite/native/host/owcompat.h")
for source in "$suite/native/host/reactor_aot.c" "$suite/native/runtime/wasm-rt-impl.c" \
  "$suite/native/runtime/wasm-rt-exceptions-impl.c" "$suite/native/host/raster.c" \
  "$suite/native/host/surface.c" "$suite/qa/native-engine/workflow.c" \
  "$suite/qa/reclaim/engine-protected-os2.c"; do
  name=$(basename "$source" .c)
  extra=()
  if [[ $name == wasm-rt-impl ]]; then extra=(-d__OS2__); fi
  "$WATCOM/binl64/wcc386" "${flags[@]}" "${extra[@]}" "-fo=$build/$name.obj" "$source"
done
{
  printf 'system linux\noption quiet\noption stack=8388608\noption map=%s/probe.map\nname %s/probe\n' "$build" "$build"
  for name in workflow reactor_aot wasm-rt-impl wasm-rt-exceptions-impl raster surface engine-protected-os2; do printf 'file %s/%s.obj\n' "$build" "$name"; done
  for name in helpers word-data word-meta; do printf 'file %s/objects/%s.obj\n' "$engine" "$name"; done
  for ((i=1;i<128;i++)); do printf 'file %s/objects/word_%u.obj\n' "$engine" "$i"; done
} > "$build/probe.lnk"
"$WATCOM/binl64/wlink" "@$build/probe.lnk"
printf '%s\n' "$build/probe"
