#!/usr/bin/env bash
set -euo pipefail
suite=$(cd "$(dirname "$0")/../.." && pwd)
engine=$(realpath "${1:?Pass existing optimized Grid object directory}")
mkdir -p "${2:?Pass separate diagnostic build directory}"
build=$(realpath "$2")
: "${WATCOM:?Source toolchain/env.sh}"
export INCLUDE="$WATCOM/h:$WATCOM/h/os2"
flags=(-q -bt=os2 -3s -zp4 -mf -zastd=c99 -ot
  "-i=$suite/native/host" "-i=$suite/native/runtime" "-i=$engine/engine"
  "-fi=$suite/native/host/owcompat.h")
"$WATCOM/binl64/wcc386" "${flags[@]}" -dWC_GRIDCRAFT -dmain=craft_app_main \
  "-fo=$build/app_main.obj" "$suite/native/host/app_main.c"
"$WATCOM/binl64/wcc386" "${flags[@]}" "-fo=$build/gridfault.obj" "$suite/qa/warp-stress/gridfault.c"
{
  printf 'system os2v2_pm\noption quiet\noption stack=8388608\noption map=%s/engine.map\nname %s/GRIDCRFT.EXE\nlibrary os2386\n' "$build" "$build"
  printf 'file %s/gridfault.obj\nfile %s/app_main.obj\n' "$build" "$build"
  for name in reactor_aot wasm-rt-impl wasm-rt-exceptions-impl helpers word-data word-meta; do printf 'file %s/objects/%s.obj\n' "$engine" "$name"; done
  for ((i=1;i<128;i++)); do printf 'file %s/objects/word_%u.obj\n' "$engine" "$i"; done
} > "$build/diagnostic.lnk"
"$WATCOM/binl64/wlink" "@$build/diagnostic.lnk"
python3 "$suite/icons/bind-icon.py" GRIDCRFT "$build/GRIDCRFT.EXE" \
  --wrc "${CRAFT_WRC:-$WATCOM/binl64/wrc}"
printf '%s\n' "$build/GRIDCRFT.EXE"
