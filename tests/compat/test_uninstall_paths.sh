#!/usr/bin/env sh
# Inspect uninstall recipes without removing or installing any files.
set -e
make=make
case $(uname) in *BSD) make=gmake ;; esac
root=${lz4_root:-..}
for target in Windows_NT MINGW64_NT MSYS_NT CYGWIN_NT; do
    case $target in
        Windows*) dll=liblz4-123.dll; implib=liblz4.lib ;;
        MINGW*) dll=liblz4.dll; implib=liblz4.dll.a ;;
        MSYS*) dll=msys-lz4-123.dll; implib=liblz4.dll.a ;;
        CYGWIN*) dll=cyglz4-123.dll; implib=liblz4.dll.a ;;
    esac
    commands=$($make -s -n -C "$root/lib" uninstall TARGET_OS="$target" PREFIX=/lz4-test LIBVER_MAJOR=123)
    printf '%s\n' "$commands" | grep -Fx "rm -f /lz4-test/bin/$dll"
    printf '%s\n' "$commands" | grep -Fx "rm -f /lz4-test/lib/$implib"
done
