#!/bin/sh
# Builds GNU make for Windows (XP and later) from its source tarball with
# the 32-bit MinGW-w64 GCC, the way the source's build_w32.bat does, but
# without needing make or cmd.exe.
#
#   build-make.sh <make source dir> <build dir> <output make.exe>
#
# MINGW is the toolchain root (default /c/msys64/mingw32); GCC_FIX holds
# extra compiler flags (prepare-sdk.sh: the GCC 15 workaround).
set -e
src=$(cd "${1:?usage: build-make.sh <source> <build dir> <make.exe>}" && pwd)
out=${2:?build dir}
exe=${3:?output}
mingw=${MINGW:-/c/msys64/mingw32}
export PATH="$mingw/bin:$PATH"
w() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else
      printf '%s\n' "$1"; fi; }

mkdir -p "$out/src" "$out/lib"
# MinGW-w64's dirent has no d_type, 32-bit or 64-bit; the config only
# allows for that on 64-bit (__MINGW64__), so test for any MinGW
sed 's/^#ifndef __MINGW64__$/#ifndef __MINGW32__/' \
    "$src/src/config.h.W32" > "$out/src/config.h"
cp "$src/lib/glob.in.h" "$out/lib/glob.h"
cp "$src/lib/fnmatch.in.h" "$out/lib/fnmatch.h"

objs=
for f in src/ar src/arscan src/commands src/default src/dir src/expand \
         src/file src/function src/getopt src/getopt1 src/guile src/hash \
         src/implicit src/job src/load src/loadapi src/main src/misc \
         src/output src/read src/remake src/remote-stub src/rule \
         src/shuffle src/signame src/strcache src/variable src/version \
         src/vpath src/w32/pathstuff src/w32/w32os src/w32/compat/posixfcn \
         src/w32/subproc/misc src/w32/subproc/sub_proc \
         src/w32/subproc/w32err lib/fnmatch lib/glob lib/getloadavg; do
    o="$out/$(echo "$f" | tr / _).o"
    gcc -mthreads -Wall -std=gnu99 -O2 $GCC_FIX \
        -ffile-prefix-map="$(w "$src")=make-src" \
        -ffile-prefix-map="$src=make-src" -I"$out/src" -I"$src/src" \
        -I"$out/lib" -I"$src/lib" -I"$src/src/w32/include" \
        -DHAVE_CONFIG_H -D_WIN32_WINNT=0x0501 -DWINVER=0x0501 \
        -w -o "$o" -c "$src/$f.c"
    objs="$objs $o"
done
mkdir -p "$(dirname "$exe")"
gcc -mthreads -O2 $GCC_FIX -s -static -o "$exe" $objs \
    -lkernel32 -luser32 -ladvapi32 -lshell32 -lole32 -loleaut32 -luuid
echo "built $exe"
