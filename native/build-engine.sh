#!/usr/bin/env bash
# Compile one application reactor using the 32-bit stack ABI shared with OpenWatcom.
set -euo pipefail
kit=$(cd "$(dirname "$0")" && pwd)
engine_opt=${CRAFT_ENGINE_OPT:-O0}
case "$engine_opt" in
    O0|O1|Os) ;;
    *) printf 'CRAFT_ENGINE_OPT must be O0, O1, or Os\n' >&2; exit 2 ;;
esac
module=$(realpath "${1:?Pass the linked WASI reactor}")
mkdir -p "${2:?Pass an output directory}"
out=$(realpath "$2")
word_support=${CRAFT_WORD_SUPPORT:-"$kit/host"}
runtime="$kit/runtime"
generated="$out/engine"
objects="$out/objects"
parts=128
mkdir -p "$generated" "$objects"
: "${CRAFT_OBJCONV:?Set CRAFT_OBJCONV to the patched objconv executable}"
relocation_audit="$kit/audit-omf.py"
test -f "$relocation_audit"
export word_support runtime generated objects CRAFT_OBJCONV engine_opt relocation_audit

"${CRAFT_WASM2C:-wasm2c}" "$module" --no-debug-names --num-outputs="$parts" \
    -n word -o "$generated/word.c"
python3 "$word_support/extract-data.py" --stack-abi \
    "$generated/word_0.c" "$generated/word-meta.c" "$objects/word-data.obj"

compile_engine() {
    local input=$1 object
    local conformance=()
    if [[ $engine_opt != O0 ]]; then
        # Preserve wasm2c's NaN, rounding and recursion semantics under GCC.
        conformance=(-fno-optimize-sibling-calls -frounding-math -fsignaling-nans)
    fi
    object="$objects/$(basename "$input" .c)"
    # A failed rebuild may not leave a previous object available for linking.
    # Keep new ELF/temporary OMF on failure so the conversion can be diagnosed.
    rm -f "$object.obj" "$object.reloc.json" "$object.tmp"
    "${CRAFT_CC:-gcc}" -m32 -std=gnu99 -march=i386 -mfpmath=387 -fexcess-precision=standard \
        -mpreferred-stack-boundary=2 -mincoming-stack-boundary=2 \
        -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables \
        -fno-unwind-tables -fno-ident -fno-strict-aliasing -fno-common \
        -DNDEBUG -DWASM_RT_USE_MMAP=0 -DWASM_RT_SKIP_SIGNAL_RECOVERY=1 \
        -I"$runtime" -I"$generated" "-$engine_opt" "${conformance[@]}" \
        -c "$input" -o "$object.o"
    "$CRAFT_OBJCONV" -fomf32 -v0 "$object.o" "$object.tmp"
    python3 "$relocation_audit" "$object.o" "$object.tmp" --output "$object.reloc.json"
    mv "$object.tmp" "$object.obj"
    unlink "$object.o"
}
export -f compile_engine

python3 - "$generated" "$word_support" "$parts" <<'PY' | xargs -0 -P "${CRAFT_JOBS:-4}" -n1 bash -ec 'compile_engine "$1"' _
import os
import sys
from pathlib import Path

generated, support = map(Path, sys.argv[1:3])
sources = [generated / f"word_{part}.c" for part in range(1, int(sys.argv[3]))]
sources.extend([generated / "word-meta.c", support / "helpers.c"])
for source in sorted(sources, key=lambda path: path.stat().st_size, reverse=True):
    sys.stdout.buffer.write(os.fsencode(source) + b"\0")
PY
printf '%s\n' "$out"
