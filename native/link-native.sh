#!/usr/bin/env bash
# Build the common PM DLL or link one previously compiled native reactor.
set -euo pipefail
kit=$(cd "$(dirname "$0")" && pwd)
app=${1:?Pass host, word, deck, or grid}
mkdir -p "${2:?Pass a build directory}" "${3:?Pass a suite binary directory}"
build=$(realpath "$2")
bin=$(realpath "$3")
: "${WATCOM:?Set WATCOM to the OpenWatcom directory}"
export PATH="$WATCOM/binl64:$WATCOM/binw:$PATH"
export INCLUDE="$WATCOM/h:$WATCOM/h/os2"
mkdir -p "$build/objects"

compile() {
    local source=$1
    shift
    "$WATCOM/binl64/wcc386" -q -bt=os2 -3s -zp4 -mf -zastd=c99 -ot "$@" \
        "-i=\"$build/engine\"" "-i=\"$kit/runtime\"" "-i=\"$kit/host\"" \
        "-i=\"$kit/vendor/jitterentropy\"" \
        "-fi=\"$kit/host/owcompat.h\"" -dWASM_RT_USE_MMAP=0 -dWASM_RT_SKIP_SIGNAL_RECOVERY=1 \
        "-fo=\"$build/objects/$(basename "$source" .c).obj\"" "\"$source\""
}

cd "$build"
if [[ $app = host ]]; then
    common=(pm raster gui_files guest_services rng a11y clipboard gui_print network)
    for unit in "${common[@]}"; do compile "$kit/host/$unit.c" -bd; done
    # Timing jitter is invalid if the compiler optimizes its measured loops.
    # -od overrides the common host's -ot for every upstream library unit.
    jitter=(base gcd health noise sha3 status timer)
    for unit in "${jitter[@]}"; do
        compile "$kit/vendor/jitterentropy/src/jitterentropy-$unit.c" -bd -od;
    done
    output=CRAFTPM.DLL
    {
        printf 'system os2v2_dll initinstance terminstance\noption quiet\noption manyautodata\n'
        printf 'segment type data nonshared\nname %s\nlibrary os2386\nexport wc_pm_get_api\n' "$output"
        for unit in "${common[@]}"; do printf 'file objects/%s.obj\n' "$unit"; done
        for unit in "${jitter[@]}"; do printf 'file objects/jitterentropy-%s.obj\n' "$unit"; done
    } > craft.lnk
else
    case "$app" in
        word) output=WORDCRFT.EXE; app_define=-dWC_WORDCRAFT ;;
        deck)
            output=DECKCRFT.EXE
            app_define=-dWC_DECKCRAFT
            : "${CRAFT_MAX_PAGES:=2048}"
            ;;
        grid) output=GRIDCRFT.EXE; app_define=-dWC_GRIDCRAFT ;;
        *) printf 'Unknown app %s\n' "$app" >&2; exit 2 ;;
    esac
    compile "$kit/host/app_main.c" "$app_define"
    compile "$kit/host/reactor_aot.c" "$app_define"
    limits=()
    if [[ -n ${CRAFT_MAX_PAGES:-} ]]; then limits+=("-dWC_WASM_MAX_PAGES=$CRAFT_MAX_PAGES"); fi
    compile "$kit/runtime/wasm-rt-impl.c" "${limits[@]}"
    compile "$kit/runtime/wasm-rt-exceptions-impl.c"
    {
        printf 'system os2v2_pm\noption quiet\noption stack=8388608\n'
        printf 'option map=engine.map\nname %s\nlibrary os2386\n' "$output"
        for unit in app_main reactor_aot wasm-rt-impl wasm-rt-exceptions-impl helpers word-data word-meta; do
            printf 'file objects/%s.obj\n' "$unit"
        done
        for (( part=1; part<128; part++ )); do printf 'file objects/word_%d.obj\n' "$part"; done
    } > craft.lnk
fi

test ! -e "$output" || unlink "$output"
"$WATCOM/binl64/wlink" @craft.lnk
file -b "$output" | grep -q 'LX.*OS/2'
if [[ $app != host ]]; then
    python3 "$kit/../icons/bind-icon.py" "${output%.EXE}" "$output" \
        --wrc "${CRAFT_WRC:-$WATCOM/binl64/wrc}"
fi
cp "$output" "$bin/$output"
printf '%s\n' "$bin/$output"
