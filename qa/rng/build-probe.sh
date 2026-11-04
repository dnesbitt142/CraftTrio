#!/usr/bin/env bash
# Reproduce the OpenWatcom i386 diagnostic for Linux ABI or native OS/2.
set -euo pipefail
suite=$(cd "$(dirname "$0")/../.." && pwd)
target=${1:?Pass linux or os2}
build=${2:?Pass a build directory}
: "${WATCOM:?Set WATCOM to an OpenWatcom v2 installation}"
mkdir -p "$build"
build=$(realpath "$build")
export PATH="$WATCOM/binl64:$PATH"
export INCLUDE="$WATCOM/h"
case "$target" in
    linux) extra=(-d_SSIZE_T_DEFINED -d__OBSCURE_STREAM_INTERNALS); system=linux; output=rng-probe ;;
    os2) extra=(); system=os2v2; output=RNGTEST.EXE ;;
    *) printf 'Unknown target: %s\n' "$target" >&2; exit 2 ;;
esac
common=(-q "-bt=$target" -3s -zp4 -mf -zastd=c99 -dWC_JENT_PORT_TEST
    "${extra[@]}" "-i=$suite/native/host" "-i=$suite/native/vendor/jitterentropy"
    "-i=$suite/native/vendor/jitterentropy/src" "-fi=$suite/native/host/owcompat.h")
objects=()
for unit in base gcd health noise sha3 status timer; do
    object="$build/jitterentropy-$unit.obj"
    "$WATCOM/binl64/wcc386" "${common[@]}" -od "-fo=$object" \
        "$suite/native/vendor/jitterentropy/src/jitterentropy-$unit.c"
    objects+=("$object")
done
for unit in rng probe; do
    if [[ $unit = rng ]]; then source="$suite/native/host/rng.c"; else source="$suite/qa/rng/rng-probe.c"; fi
    object="$build/$unit.obj"
    "$WATCOM/binl64/wcc386" "${common[@]}" -ot "-fo=$object" "$source"
    objects+=("$object")
done
{
    printf 'system %s\noption quiet\noption stack=1048576\nname %s\n' "$system" "$build/$output"
    for object in "${objects[@]}"; do printf 'file %s\n' "$object"; done
} > "$build/probe.lnk"
"$WATCOM/binl64/wlink" "@$build/probe.lnk"
file "$build/$output"
if [[ -n ${3:-} ]]; then
    mkdir -p "$3"
    destination=$(realpath "$3")
    if [[ "$destination/$output" != "$build/$output" ]]; then
        cp "$build/$output" "$destination/$output"
    fi
    printf '%s\n' "$destination/$output"
else
    printf '%s\n' "$build/$output"
fi
