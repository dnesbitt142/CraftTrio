#!/usr/bin/env bash
set -euo pipefail
source_root=$(cd "$(dirname "$0")" && pwd)
suite=$(cd "$source_root/../../.." && pwd)
mkdir -p "${1:?Pass an output directory}"
build=$(realpath "$1")
: "${WATCOM:?Source toolchain/env.sh}"
: "${CRAFT_OBJCONV:?Source toolchain/env.sh}"
export INCLUDE="$WATCOM/h"
"${CRAFT_WASM2C%/*}/wat2wasm" "$source_root/numeric.wat" -o "$build/numeric.wasm"
"${CRAFT_WASM2C}" "$build/numeric.wasm" -n numeric -o "$build/numeric.c"
python3 "$source_root/cases.py" "$build/cases.h"
for mode in O0 O1 Os; do
  mkdir -p "$build/$mode"
  extra=()
  if [[ $mode != O0 ]]; then extra=(-fno-optimize-sibling-calls -frounding-math -fsignaling-nans); fi
  for input in "$build/numeric.c" "$suite/native/host/helpers.c"; do
    name=$(basename "$input" .c)
    "$CRAFT_CC" -m32 -std=gnu99 -march=i386 -mfpmath=387 -fexcess-precision=standard \
      -mpreferred-stack-boundary=2 -mincoming-stack-boundary=2 \
      -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables \
      -fno-unwind-tables -fno-ident -fno-strict-aliasing -fno-common \
      -DNDEBUG -DWASM_RT_USE_MMAP=0 -DWASM_RT_SKIP_SIGNAL_RECOVERY=1 \
      "-I$suite/native/runtime" "-I$build" "-$mode" "${extra[@]}" -c "$input" -o "$build/$mode/$name.o"
    "$CRAFT_OBJCONV" -fomf32 -v0 "$build/$mode/$name.o" "$build/$mode/$name.obj"
  done
  for input in "$source_root/driver.c" "$suite/native/runtime/wasm-rt-impl.c" "$suite/native/runtime/wasm-rt-exceptions-impl.c"; do
    name=$(basename "$input" .c)
    "$WATCOM/binl64/wcc386" -q -bt=linux -3s -zp4 -mf -zastd=c99 -ot \
      -d_SSIZE_T_DEFINED -d__OBSCURE_STREAM_INTERNALS \
      -dWASM_RT_USE_MMAP=0 -dWASM_RT_SKIP_SIGNAL_RECOVERY=1 \
      "-i=$suite/native/host" "-i=$suite/native/runtime" "-i=$build" \
      "-fi=$suite/native/host/owcompat.h" "-fo=$build/$mode/$name.obj" "$input"
  done
  {
    printf 'system linux\noption quiet\noption stack=8388608\nname %s/numeric-probe\n' "$build/$mode"
    for name in driver numeric helpers wasm-rt-impl wasm-rt-exceptions-impl; do printf 'file %s/%s.obj\n' "$build/$mode" "$name"; done
  } > "$build/$mode/probe.lnk"
  "$WATCOM/binl64/wlink" "@$build/$mode/probe.lnk"
  "$suite/../toolchain/usr/bin/qemu-i386-static" -cpu pentium "$build/$mode/numeric-probe" | tee "$build/$mode/results.log"
done
for mode in O1 Os; do cmp "$build/O0/results.log" "$build/$mode/results.log"; done
