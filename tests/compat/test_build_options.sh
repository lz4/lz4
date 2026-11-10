#!/usr/bin/env sh
# lib/ : BUILD_STATIC=no and BUILD_SHARED=no build and install only the other library.
# Each build starts from a clean lib/ : a missing rule only fails when its target is absent.
set -e

make='make'
case $(uname) in *BSD) make=gmake ;; esac
lib="$lz4_root/lib"
staging="$(pwd)/tmp-build-options"

# lists the libraries present in directory $1
libs() {
  (cd "$1" && ls liblz4.a liblz4.so* liblz4*.dylib 2>/dev/null || true) | tr '\n' ' '
}

for opt in BUILD_STATIC BUILD_SHARED; do
  rm -rf "$staging"
  $make -C "$lib" clean >/dev/null
  echo "$make -C $lib $opt=no ; $make -C $lib install $opt=no"
  $make -C "$lib" $opt=no >/dev/null
  $make -C "$lib" install $opt=no DESTDIR="$staging" LIBDIR=/lib >/dev/null
  for dir in "$lib" "$staging/lib"; do
    found=$(libs "$dir")
    echo "  $dir : $found"
    case $opt in
      BUILD_STATIC) case "$found" in ''|*liblz4.a*) echo "error : expected only the dynamic library"; exit 1 ;; esac ;;
      BUILD_SHARED) [ "$found" = "liblz4.a " ] || { echo "error : expected only liblz4.a"; exit 1; } ;;
    esac
  done
done

$make -C "$lib" clean >/dev/null
rm -rf "$staging"
