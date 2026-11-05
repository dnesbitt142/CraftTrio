#!/usr/bin/env bash
set -euo pipefail
suite=$(cd "$(dirname "$0")/../.." && pwd)
engine=$(realpath "${1:?Pass engine build directory}")
mkdir -p "${2:?Pass separate QA build directory}"
build=$(realpath "$2")
app=${3:-grid}
probe_source=${CRAFT_QA_PROBE_SOURCE:-"$suite/qa/native-engine/probe.c"}
: "${WATCOM:?Set WATCOM}"
export PATH="$WATCOM/binl64:$PATH"
export INCLUDE="$WATCOM/h"
flags=(-q -bt=linux -3s -zp4 -mf -zastd=c99 -ot -d_SSIZE_T_DEFINED -d__OBSCURE_STREAM_INTERNALS
  -dWASM_RT_USE_MMAP=0 -dWASM_RT_SKIP_SIGNAL_RECOVERY=1
  "-i=$suite/native/host" "-i=$suite/native/runtime" "-i=$engine/engine"
  "-fi=$suite/native/host/owcompat.h")
if [[ $app = deck ]]; then flags+=(-dWC_DECKCRAFT); fi
for source in "$suite/native/host/reactor_aot.c" "$suite/native/runtime/wasm-rt-impl.c" \
  "$suite/native/runtime/wasm-rt-exceptions-impl.c" "$suite/native/host/raster.c" \
  "$suite/native/host/surface.c" "$probe_source"; do
  name=$(basename "$source" .c)
  if [[ $source = "$probe_source" ]]; then name=probe; fi
  "$WATCOM/binl64/wcc386" "${flags[@]}" "-fo=$build/$name.obj" "$source"
done
{
  printf 'system linux\noption quiet\noption stack=8388608\noption map=%s/probe.map\nname %s/probe\n' "$build" "$build"
  for name in probe reactor_aot wasm-rt-impl wasm-rt-exceptions-impl raster surface; do printf 'file %s/%s.obj\n' "$build" "$name"; done
  for name in helpers word-data word-meta; do printf 'file %s/objects/%s.obj\n' "$engine" "$name"; done
  for ((i=1;i<128;i++)); do printf 'file %s/objects/word_%u.obj\n' "$engine" "$i"; done
} > "$build/probe.lnk"
"$WATCOM/binl64/wlink" "@$build/probe.lnk"
printf '%s\n' "$build/probe"
