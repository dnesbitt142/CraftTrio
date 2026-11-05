#!/usr/bin/env bash
# Cross-build the numerical/trap corpus for execution in an actual OS/2 guest.
set -euo pipefail
source_root=$(cd "$(dirname "$0")" && pwd)
suite=$(cd "$source_root/../../.." && pwd)
mkdir -p "${1:?Pass a fresh output directory}"
build=$(realpath "$1")
: "${WATCOM:?Source toolchain/env.sh}"
: "${CRAFT_OBJCONV:?Source toolchain/env.sh}"
export INCLUDE="$WATCOM/h:$WATCOM/h/os2"
"${CRAFT_WASM2C%/*}/wat2wasm" "$source_root/numeric.wat" -o "$build/numeric.wasm"
"${CRAFT_WASM2C}" "$build/numeric.wasm" -n numeric -o "$build/numeric.c"
python3 "$source_root/cases.py" "$build/cases.h"
for mode in O0 O1 Os; do
  mkdir -p "$build/$mode"
  case "$mode" in O0) exe=NUMO0;; O1) exe=NUMO1;; Os) exe=NUMOS;; esac
  extra=()
  if [[ $mode != O0 ]]; then extra=(-fno-optimize-sibling-calls -frounding-math -fsignaling-nans); fi
  for input in "$build/numeric.c" "$suite/native/host/helpers.c"; do
    name=$(basename "$input" .c)
    "$CRAFT_CC" -m32 -std=gnu99 -march=i386 -mfpmath=387 -fexcess-precision=standard \
      -mpreferred-stack-boundary=2 -mincoming-stack-boundary=2 \
      -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables \
      -fno-unwind-tables -fno-ident -fno-strict-aliasing -fno-common \
      -DNDEBUG -DWASM_RT_USE_MMAP=0 -DWASM_RT_SKIP_SIGNAL_RECOVERY=1 \
      "-I$suite/native/runtime" "-I$build" "-$mode" "${extra[@]}" \
      -c "$input" -o "$build/$mode/$name.o"
    "$CRAFT_OBJCONV" -fomf32 -v0 "$build/$mode/$name.o" "$build/$mode/$name.obj"
  done
  for input in "$source_root/driver.c" "$suite/native/runtime/wasm-rt-impl.c" "$suite/native/runtime/wasm-rt-exceptions-impl.c"; do
    name=$(basename "$input" .c)
    "$WATCOM/binl64/wcc386" -q -bt=os2 -3s -zp4 -mf -zastd=c99 -ot \
      -dWASM_RT_USE_MMAP=0 -dWASM_RT_SKIP_SIGNAL_RECOVERY=1 \
      "-i=$suite/native/host" "-i=$suite/native/runtime" "-i=$build" \
      "-fi=$suite/native/host/owcompat.h" "-fo=$build/$mode/$name.obj" "$input"
  done
  {
    printf 'system os2v2\noption quiet\noption stack=8388608\nname %s/%s.EXE\nlibrary os2386\n' "$build/$mode" "$exe"
    for name in driver numeric helpers wasm-rt-impl wasm-rt-exceptions-impl; do printf 'file %s/%s.obj\n' "$build/$mode" "$name"; done
  } > "$build/$mode/numeric.lnk"
  "$WATCOM/binl64/wlink" "@$build/$mode/numeric.lnk"
done
printf 'Cross-built only; run NUMO0.EXE, NUMO1.EXE and NUMOS.EXE without arguments in OS/2.\n'
