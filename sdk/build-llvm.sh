#!/bin/sh
# Builds clang, lld, lldb and a few binary utilities as 32-bit Windows
# programs that run on Windows XP, with the X86 and AMDGPU targets.
#
#   build-llvm.sh <llvm-project source> <build dir>
#
# The programs end up in <build dir>/bin; prepare-sdk.sh picks them up.
# The source must have the patches in patches/llvm/ applied (prepare-sdk.sh
# does that).
#
# Environment:
#   MINGW       32-bit MinGW-w64 GCC root (msvcrt, posix threads), default
#               /c/msys64/mingw32
#   CMAKE, NINJA, PYTHON   the tools to use (default: from PATH)
#   LLD_LINKER  optional 64-bit ld.lld to link with; a 32-bit ld.bfd can run
#               out of address space linking clang
#   JOBS        parallel jobs (default: all processors)
#   BUILD_TARGETS  what to build (default: everything the SDK needs)
#   GCC_FIX     extra compiler flags (prepare-sdk.sh: the GCC 15 workaround,
#               see gcc-check.c)
set -e
src=$(cd "${1:?usage: build-llvm.sh <source> <build dir>}" && pwd)
build=${2:?build dir}
mingw=${MINGW:-/c/msys64/mingw32}
cmake=${CMAKE:-cmake}
ninja=${NINJA:-ninja}
python=${PYTHON:-python3}
jobs=${JOBS:-$(nproc)}
w() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else
      printf '%s\n' "$1"; fi; }

export PATH="$mingw/bin:$PATH"
# XP's loader refuses executables that ask for a newer OS or subsystem
# version; lld writes 6.0 (Vista) unless told otherwise
xp="-static -Wl,--major-os-version=5,--minor-os-version=1"
xp="$xp,--major-subsystem-version=5,--minor-subsystem-version=1"
# Source and build paths (assertion messages, logging) start at the source
# directory's name instead of this machine's directories.  With
# LLVM_APPEND_VC_REV off, version strings name no repository either.
mkdir -p "$build"
prefix_map="-ffile-prefix-map=$(w "$src")=$(basename "$src")"
prefix_map="$prefix_map -ffile-prefix-map=$(w "$(cd "$build" && pwd)")=build"
linker_args=
if [ -n "$LLD_LINKER" ]; then
    # gcc runs "ld.lld" from PATH for -fuse-ld=lld
    mkdir -p "$build/lld-bin"
    cp "$LLD_LINKER" "$build/lld-bin/ld.lld.exe"
    export PATH="$build/lld-bin:$PATH"
    linker_args="-DLLVM_USE_LINKER=lld"
fi

"$cmake" -S "$(w "$src/llvm")" -B "$(w "$build")" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$(w "$(command -v "$ninja")")" \
    -DPython3_EXECUTABLE="$(w "$(command -v "$python")")" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="$GCC_FIX $prefix_map" \
    -DCMAKE_CXX_FLAGS="$GCC_FIX $prefix_map" \
    -DLLVM_APPEND_VC_REV=OFF \
    -DCMAKE_C_COMPILER="$(w "$mingw/bin/gcc.exe")" \
    -DCMAKE_CXX_COMPILER="$(w "$mingw/bin/g++.exe")" \
    -DCMAKE_RC_COMPILER="$(w "$mingw/bin/windres.exe")" \
    -DCMAKE_EXE_LINKER_FLAGS="$xp -Wl,--large-address-aware" \
    -DCMAKE_SHARED_LINKER_FLAGS="$xp" \
    -DLLVM_ENABLE_PROJECTS="clang;lld;lldb" \
    -DLLVM_TARGETS_TO_BUILD="X86;AMDGPU" \
    -DLLVM_DEFAULT_TARGET_TRIPLE=i686-w64-windows-gnu \
    -DLLVM_HOST_TRIPLE=i686-w64-windows-gnu \
    -DLLVM_ENABLE_ZLIB=OFF \
    -DLLVM_ENABLE_ZSTD=OFF \
    -DLLVM_ENABLE_LIBXML2=OFF \
    -DLLVM_ENABLE_LIBEDIT=OFF \
    -DLLVM_ENABLE_TERMINFO=OFF \
    -DLLVM_INCLUDE_TESTS=OFF \
    -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DLLVM_INCLUDE_EXAMPLES=OFF \
    -DLLVM_INCLUDE_DOCS=OFF \
    -DLLVM_ENABLE_PLUGINS=OFF \
    -DCLANG_ENABLE_STATIC_ANALYZER=OFF \
    -DCLANG_PLUGIN_SUPPORT=OFF \
    -DLLDB_ENABLE_PYTHON=OFF \
    -DLLDB_ENABLE_LUA=OFF \
    -DLLDB_ENABLE_LIBEDIT=OFF \
    -DLLDB_ENABLE_CURSES=OFF \
    -DLLDB_ENABLE_LIBXML2=OFF \
    -DLLDB_ENABLE_LZMA=OFF \
    -DLLDB_INCLUDE_TESTS=OFF \
    $linker_args

targets=${BUILD_TARGETS:-clang lld lldb llvm-ar llvm-objdump llvm-readobj \
    llvm-nm llvm-objcopy clang-resource-headers}
"$cmake" --build "$(w "$build")" -j "$jobs" --target $targets
