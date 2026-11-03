#!/bin/sh
compiler=$1
shift
case " $* " in
    *' --target wasm32-wasip1 '*|*' --target=wasm32-wasip1 '*)
        exec "$compiler" "$@"
        ;;
    *)
        exec "$compiler" -Clinker-features=-lld "$@"
        ;;
esac
