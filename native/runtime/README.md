# WABT support for OS/2

These five source and header files come from WebAssembly Binary Toolkit (WABT) 1.0.34, packaged as Ubuntu `wabt` version `1.0.34+dsfg2+~cs1.0.32-1ubuntu2`. The package used for this build has SHA-256 `ac76092c4ce135ea946f3f9b60b13cd0e1cbbbb8ae96c2bf4838b45950e833aa`. The files retain their Apache-2.0 notices. [WABT 1.0.34 upstream source](https://github.com/WebAssembly/wabt/tree/1.0.34/wasm2c)

`wabt-1.0.34-os2.patch` records the changes to `wasm-rt-impl.c`. Apply it with `patch -p1` from the WABT source root, or from the package's extracted `usr/src` directory. The patch gates host mapping code behind the corresponding configuration flags and reserves up to 96 MiB of OS/2 address space with `DosAllocMem` and `OBJ_ANY`. The kernel may place the reservation in high memory. `DosSetMem` commits the initial range and commits more pages on growth. A failed initial allocation produces a catchable trap, and subsequent allocation errors are logged to `WCENGINE.LOG`.

## Compiler settings

The build script uses the same headers for the translated engine and its OS/2 host. The host and support files use OpenWatcom with `-bt=os2 -3s -zp4 -mf -zastd=c99 -ot`, the OS/2 include directories, and the forced include `host/owcompat.h`. The translated C uses GCC i386 stack calling conventions with `-m32 -march=i386 -mfpmath=387 -fexcess-precision=standard -mpreferred-stack-boundary=2 -mincoming-stack-boundary=2`. The complete flags and OMF conversion are in [build-engine.sh](../build-engine.sh) and [link-native.sh](../link-native.sh).

Both builds define `WASM_RT_USE_MMAP=0` and `WASM_RT_SKIP_SIGNAL_RECOVERY=1`. They use explicit bounds checks and stack-depth checks. The support code uses OS/2 memory objects for linear memory. The linked support code handles memory, function tables, and traps for the generated native functions.
