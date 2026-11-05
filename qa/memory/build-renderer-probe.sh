#!/usr/bin/env bash
# Build a standalone renderer comparison from current and previous source.
set -euo pipefail
suite=$(cd "$(dirname "$0")/../.." && pwd)
baseline=$(realpath "${1:?Pass previous native/host source directory}")
mkdir -p "${2:?Pass output directory}"
build=$(realpath "$2")
target=${3:-linux}
: "${WATCOM:?Set WATCOM to OpenWatcom v2}"
export PATH="$WATCOM/binl64:$PATH"
export INCLUDE="$WATCOM/h"
case "$target" in
    linux) extra=(-d_SSIZE_T_DEFINED -d__OBSCURE_STREAM_INTERNALS); system=linux; output=renderer-probe ;;
    os2) extra=(); system=os2v2; output=MEMTEST.EXE ;;
    *) exit 1 ;;
esac
common=(-q "-bt=$target" -3s -zp4 -mf -zastd=c99 -ot "${extra[@]}"
    "-i=$suite/native/host" "-fi=$suite/native/host/owcompat.h")
rename=(-dWcRaster=BaseRaster)
for name in create destroy frame pixels error; do rename+=("-dwc_raster_$name=base_wc_raster_$name");done
"$WATCOM/binl64/wcc386" "${common[@]}" "${rename[@]}" "-fo=$build/baseline.obj" "$baseline/raster.c"
for name in raster surface; do
    "$WATCOM/binl64/wcc386" "${common[@]}" "-fo=$build/$name.obj" "$suite/native/host/$name.c"
done
"$WATCOM/binl64/wcc386" "${common[@]}" "-fo=$build/probe.obj" "$suite/qa/memory/renderer-probe.c"
{
    printf 'system %s\noption quiet\noption stack=1048576\nname %s\n' "$system" "$build/$output"
    for name in baseline raster surface probe; do printf 'file %s/%s.obj\n' "$build" "$name";done
} > "$build/probe.lnk"
"$WATCOM/binl64/wlink" "@$build/probe.lnk"
printf '%s\n' "$build/$output"
