#!/usr/bin/env bash
set -euo pipefail
source_root=$(cd "$(dirname "$0")" && pwd)
suite=$(cd "$source_root/../../.." && pwd)
mkdir -p "${1:?Pass output directory}"
build=$(realpath "$1")
classifier_source=${2:-"$suite/native/host/helpers.c"}
export INCLUDE="$WATCOM/h"
for mode in O0 O1 Os; do
  mkdir -p "$build/$mode"
  extra=()
  if [[ $mode != O0 ]]; then extra=(-fno-optimize-sibling-calls -frounding-math -fsignaling-nans); fi
  for name in classifier-candidate classifier-call; do
    input="$source_root/$name.c"
    if [[ $name = classifier-candidate ]]; then input=$classifier_source; fi
    "$CRAFT_CC" -m32 -std=gnu99 -march=i386 -mfpmath=387 -fexcess-precision=standard \
      -mpreferred-stack-boundary=2 -mincoming-stack-boundary=2 \
      -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables \
      -fno-unwind-tables -fno-ident -fno-strict-aliasing -fno-common \
      -DNDEBUG "-$mode" "${extra[@]}" -c "$input" -o "$build/$mode/$name.o"
    "$CRAFT_OBJCONV" -fomf32 -v0 "$build/$mode/$name.o" "$build/$mode/$name.obj"
  done
  "$WATCOM/binl64/wcc386" -q -bt=linux -3s -zp4 -mf -zastd=c99 -ot \
    -d_SSIZE_T_DEFINED -d__OBSCURE_STREAM_INTERNALS "-fo=$build/$mode/driver.obj" "$source_root/classifier-driver.c"
  {
    printf 'system linux\noption quiet\nname %s/classifier-probe\n' "$build/$mode"
    for name in driver classifier-candidate classifier-call; do printf 'file %s/%s.obj\n' "$build/$mode" "$name"; done
  } > "$build/$mode/probe.lnk"
  "$WATCOM/binl64/wlink" "@$build/$mode/probe.lnk"
  "$suite/../toolchain/usr/bin/qemu-i386-static" -cpu pentium "$build/$mode/classifier-probe" | tee "$build/$mode/results.log"
done
for mode in O1 Os; do cmp "$build/O0/results.log" "$build/$mode/results.log"; done
