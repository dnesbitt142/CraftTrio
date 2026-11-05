#!/usr/bin/env bash
# Build the same mask-cache comparison with GCC sanitizers or OpenWatcom.
set -euo pipefail
suite=$(cd "$(dirname "$0")/../.." && pwd)
baseline=$(realpath "${1:?Pass memory1 native/host source directory}")
mkdir -p "${2:?Pass build directory}"
build=$(realpath "$2")
target=${3:-sanitizer}
rename=(-DWcRaster=BaseRaster)
for name in create destroy frame pixels error memory; do
    rename+=("-Dwc_raster_$name=base_wc_raster_$name")
done
for name in resize clear trim destroy; do
    rename+=("-Dwc_surface_$name=base_wc_surface_$name")
done
if [[ $target = sanitizer ]]; then
    cc=${CC:-gcc}
    flags=(-std=c99 -O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
    for name in raster surface; do
        "$cc" "${flags[@]}" "${rename[@]}" -I "$baseline" -c "$baseline/$name.c" -o "$build/base-$name.o"
        "$cc" "${flags[@]}" -include "$suite/qa/memory/alloc-shim.h" -I "$suite/native/host" \
            -c "$suite/native/host/$name.c" -o "$build/$name.o"
    done
    "$cc" "${flags[@]}" -DQA_ALLOC_TEST -I "$suite/native/host" \
        "$suite/qa/memory2/mask-probe.c" "$suite/qa/memory/alloc-shim.c" \
        "$build/base-raster.o" "$build/base-surface.o" "$build/raster.o" "$build/surface.o" \
        -o "$build/mask-probe"
    ASAN_OPTIONS=detect_leaks=0 "$build/mask-probe"
else
    : "${WATCOM:?Set WATCOM to OpenWatcom v2}"
    export PATH="$WATCOM/binl64:$PATH"
    export INCLUDE="$WATCOM/h"
    case "$target" in
        linux) extra=(-d_SSIZE_T_DEFINED -d__OBSCURE_STREAM_INTERNALS); system=linux; output=mask-probe ;;
        os2) extra=(); system=os2v2; output=MASKTEST.EXE ;;
        *) exit 1 ;;
    esac
    flags=(-q "-bt=$target" -3s -zp4 -mf -zastd=c99 -ot "${extra[@]}")
    watcom_rename=()
    for define in "${rename[@]}"; do watcom_rename+=("${define/-D/-d}"); done
    for name in raster surface; do
        "$WATCOM/binl64/wcc386" "${flags[@]}" "${watcom_rename[@]}" "-i=$baseline" \
            "-fi=$suite/native/host/owcompat.h" "-fo=$build/base-$name.obj" "$baseline/$name.c"
        "$WATCOM/binl64/wcc386" "${flags[@]}" "-i=$suite/native/host" \
            "-fi=$suite/qa/memory2/mask-probe-prefix.h" "-fo=$build/$name.obj" "$suite/native/host/$name.c"
    done
    "$WATCOM/binl64/wcc386" "${flags[@]}" -dQA_ALLOC_TEST "-i=$suite/native/host" \
        "-fi=$suite/native/host/owcompat.h" "-fo=$build/probe.obj" "$suite/qa/memory2/mask-probe.c"
    "$WATCOM/binl64/wcc386" "${flags[@]}" "-fi=$suite/native/host/owcompat.h" \
        "-fo=$build/alloc.obj" "$suite/qa/memory/alloc-shim.c"
    {
        printf 'system %s\noption quiet\noption stack=1048576\nname %s\n' "$system" "$build/$output"
        for name in base-raster base-surface raster surface probe alloc; do
            printf 'file %s/%s.obj\n' "$build" "$name"
        done
    } > "$build/probe.lnk"
    "$WATCOM/binl64/wlink" "@$build/probe.lnk"
    printf '%s\n' "$build/$output"
fi
