#!/usr/bin/env bash
# Build/run ASan+UBSan tests, including allocation-failure rollback.
set -euo pipefail
suite=$(cd "$(dirname "$0")/../.." && pwd)
baseline=$(realpath "${1:?Pass previous native/host source directory}")
mkdir -p "${2:?Pass output directory}"
build=$(realpath "$2")
cc=${CC:-gcc}
common=(-std=c99 -O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
rename=(-DWcRaster=BaseRaster)
for name in create destroy frame pixels error;do rename+=("-Dwc_raster_$name=base_wc_raster_$name");done
"$cc" "${common[@]}" "${rename[@]}" -I "$baseline" -c "$baseline/raster.c" -o "$build/base.o"
for name in raster surface;do
    "$cc" "${common[@]}" -include "$suite/qa/memory/alloc-shim.h" -I "$suite/native/host" \
        -c "$suite/native/host/$name.c" -o "$build/$name.o"
done
"$cc" "${common[@]}" -DQA_ALLOC_TEST -I "$suite/native/host" -I "$suite/qa/memory" \
    "$suite/qa/memory/renderer-probe.c" "$suite/qa/memory/alloc-shim.c" \
    "$build/base.o" "$build/raster.o" "$build/surface.o" -o "$build/renderer-probe"
ASAN_OPTIONS=detect_leaks=0 "$build/renderer-probe"
