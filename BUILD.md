# Build Craft Trio for OS/2

The three programs run the upstream Rust engines and egui interfaces. Rust emits WASI modules on the build machine. WABT 1.0.34 translates them to C, GCC emits 32-bit x86 machine code, and OpenWatcom compiles the OS/2 host and links the LX applications and DLL. The installed programs execute the compiled instructions directly.

## Source layout

Install Rust 1.95.0 with the `wasm32-wasip1` target. Check out the repositories next to this kit's source directories.

| Checkout directory | Repository | Revision |
| --- | --- | --- |
| `gui-upstream` | [WordCraft](https://github.com/storytold/wordcraft) | `866c1f9cac17ff12083215b8d0246c48034617e6` |
| `gui-craft-libs` | [craft-libs](https://github.com/storytold/craft-libs) | `92d392cff132eaf53e13afff77f38b5c11b73aa0` |
| `deck-upstream` | [DeckCraft](https://github.com/storytold/deckcraft) | `0c00f2240352b17cba571e93573af48e017a058d` |
| `grid-upstream` | [GridCraft](https://github.com/storytold/gridcraft) | `178f3eb745a589c63b61ef97dd7b6b2d6fd48515` |
| `grid-craft-libs` | [craft-libs](https://github.com/storytold/craft-libs) | `c5a1cbfb5f9049f9270955caffb5bee5997aebf7` |

From the kit root, apply the patches to their corresponding clean checkouts.

```sh
kit=$PWD
git -C gui-upstream apply "$kit/patches/wordcraft.patch"
git -C gui-craft-libs apply "$kit/patches/craft-libs.patch"
git -C deck-upstream apply "$kit/patches/deck-upstream-wasi.patch"
git -C grid-upstream apply "$kit/patches/grid-upstream-wasi.patch"
git -C grid-craft-libs apply "$kit/patches/grid-craft-libs-wasi.patch"
```

The DeckCraft bridge lives in `deck-bridge`. The other two bridge crates are added by their upstream patches. Build each reactor.

```sh
(cd gui-upstream && CARGO_PROFILE_DEV_DEBUG=0 cargo +1.95.0 rustc --locked \
    --target wasm32-wasip1 -p wordcraft-warp --lib -- -C strip=symbols)
(cd deck-bridge && cargo +1.95.0 rustc --locked --release \
    --target wasm32-wasip1 --lib -- -C strip=symbols)
(cd grid-upstream && CARGO_PROFILE_RELEASE_OPT_LEVEL=z \
    CARGO_PROFILE_RELEASE_CODEGEN_UNITS=16 CARGO_PROFILE_RELEASE_PANIC=abort \
    cargo +1.95.0 rustc --locked --release --target wasm32-wasip1 \
    -p gridcraft-warp --lib -- -C strip=symbols)
```

If the host Rust linker cannot locate itself in a restricted sandbox, use the absolute path to `native/host/host-rustc.sh` as `RUSTC_WRAPPER`.

## Compile the LX programs

Install GCC with 32-bit headers, Python 3, WABT 1.0.34, and OpenWatcom with its OS/2 headers and libraries. Apply `patches/objconv-elf-bounds.patch` to [objconv](https://www.agner.org/optimize/#objconv) and rebuild it. The patch corrects a relocation bounds check on large generated objects.

```sh
export WATCOM=/absolute/path/to/openwatcom
export CRAFT_OBJCONV=/absolute/path/to/patched/objconv
export CRAFT_JOBS=4
native/build-engine.sh gui-upstream/target/wasm32-wasip1/debug/wordcraft_warp.wasm build/word
native/build-engine.sh deck-bridge/target/wasm32-wasip1/release/deckcraft_warp.wasm build/deck
native/build-engine.sh grid-upstream/target/wasm32-wasip1/release/gridcraft_warp.wasm build/grid
native/link-native.sh host build/host build/bin
native/link-native.sh word build/word build/bin
native/link-native.sh deck build/deck build/bin
native/link-native.sh grid build/grid build/bin
```

`CRAFT_WASM2C` and `CRAFT_WRC` select specific WABT and OpenWatcom tools. The build needs several gigabytes of scratch space for generated C and object files. Native engine code uses the i386 stack calling convention and OpenWatcom uses `-3s -zp4` at the boundary. Each editor owns its engine state. The shared host exposes a versioned function table and keeps writable state private to each OS/2 process.

The bundled EXE compressor uses the OS/2 loader's EXEPACK page format. Compress the executables after binding their icons.

```sh
mkdir -p build/packed
for exe in WORDCRFT.EXE DECKCRFT.EXE GRIDCRFT.EXE; do
    python3 native/pack-lx.py "build/bin/$exe" "build/packed/$exe"
done
cp build/bin/CRAFTPM.DLL build/packed/
```

See [the WABT runtime notes](native/runtime/README.md) for the arena and bounds checks.
