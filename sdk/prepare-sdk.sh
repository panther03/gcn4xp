#!/bin/sh
# prepare-sdk.sh - builds the gcn4xp SDK archive (gcn4xp_sdk.zip): a
# toolchain and runtime for writing GPU programs on Windows XP itself.
#
#   sh sdk/prepare-sdk.sh
#
# Runs from a fresh clone on a current Windows machine, in a POSIX shell
# (MSYS2's or Git's).  The only requirement is a 32-bit MinGW-w64 GCC that
# uses msvcrt and posix threads, e.g. MSYS2's mingw-w64-i686-gcc; MINGW
# names its root (default /c/msys64/mingw32).  Everything else is
# downloaded, checked against pinned hashes, and kept under build/sdk/
# (untracked), so a second run only redoes what changed.
#
# Environment:
#   MINGW    toolchain root
#   JOBS     parallel build jobs (default: all processors)
#   XP_DLLS  optional: a directory with Windows XP's system DLLs; every
#            program in the SDK is then checked against them
#   OUT      the archive to write (default build/gcn4xp_sdk.zip)
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
work="$repo/build/sdk"
deps="$work/deps"
stage="$work/stage/gcn4xp_sdk"
out=${OUT:-$repo/build/gcn4xp_sdk.zip}
export MINGW=${MINGW:-/c/msys64/mingw32}
jobs=${JOBS:-$(nproc 2>/dev/null || echo 4)}

say() { printf '\n== %s\n' "$*"; }
die() { printf 'prepare-sdk: %s\n' "$*" >&2; exit 1; }
# native Windows programs want C:/... paths
w() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else
      printf '%s\n' "$1"; fi; }

# ---- what we use from Windows itself ----------------------------------------

sysroot_win=${SYSTEMROOT:-${SystemRoot:-C:/Windows}}
sys32=$(if command -v cygpath >/dev/null 2>&1; then cygpath -u "$sysroot_win";
        else printf '%s' "$sysroot_win"; fi)/System32
BSDTAR="$sys32/tar.exe"     # libarchive: tar.xz, tar.gz and zip
CURL="$sys32/curl.exe"
[ -x "$BSDTAR" ] || die "no $BSDTAR (Windows 10 1803 or later has it)"
command -v curl >/dev/null 2>&1 && CURL=curl
command -v "$CURL" >/dev/null 2>&1 || [ -x "$CURL" ] || die "no curl"

sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        certutil -hashfile "$(w "$1")" SHA256 | sed -n 2p | tr -d ' \r'
    fi
}

# ---- the toolchain we build with ------------------------------------------

[ -x "$MINGW/bin/gcc.exe" ] || die "no MinGW GCC in $MINGW (set MINGW)"
for t in g++ ar strip windres; do
    [ -x "$MINGW/bin/$t.exe" ] || die "no $t in $MINGW/bin"
done
machine=$("$MINGW/bin/gcc.exe" -dumpmachine)
[ "$machine" = i686-w64-mingw32 ] ||
    die "$MINGW is a $machine toolchain, need i686-w64-mingw32"
"$MINGW/bin/gcc.exe" -v 2>&1 | grep -q "Thread model: posix" ||
    die "the toolchain must use the posix thread model"
if echo '#include <stdio.h>' | "$MINGW/bin/gcc.exe" -dM -E -x c - |
       grep -q '_UCRT'; then
    die "the toolchain uses UCRT, which XP does not have; need msvcrt"
fi
gccver=$("$MINGW/bin/gcc.exe" -dumpversion)
# gcc's own programs need the DLLs next to it
export PATH="$MINGW/bin:$PATH"

# GCC 15's late combine pass miscompiles some 64-bit arithmetic on i686
# (see gcc-check.c).  Everything built with GCC here gets GCC_FIX, and the
# compiler must pass the check with it.
GCC_FIX=
if echo 'int x;' | gcc -fno-late-combine-instructions -fsyntax-only -x c - \
       2>/dev/null; then
    GCC_FIX=-fno-late-combine-instructions
fi
export GCC_FIX
mkdir -p "$work"
gcc -O2 $GCC_FIX -o "$(w "$work/gcc-check.exe")" "$(w "$here/gcc-check.c")" &&
    "$work/gcc-check.exe" ||
    die "$MINGW's gcc miscompiles sdk/gcc-check.c even with '$GCC_FIX'"

# ---- downloads ----------------------------------------------------------------

LLVM_VER=21.1.8
CMAKE_VER=3.31.6
NINJA_VER=1.12.1
PYTHON_VER=3.12.8
MAKE_VER=4.4.1
FIRMWARE_TAG=20261019
fw_url="https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain"

# name  url  sha256
DEPS="
llvm-project-$LLVM_VER.src.tar.xz https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/llvm-project-$LLVM_VER.src.tar.xz 4633a23617fa31a3ea51242586ea7fb1da7140e426bd62fc164261fe036aa142
cmake-$CMAKE_VER-windows-x86_64.zip https://github.com/Kitware/CMake/releases/download/v$CMAKE_VER/cmake-$CMAKE_VER-windows-x86_64.zip d163cd3ab4959b0a53fa8988f2ddbd2e6c501658201e6a154386bad9dbe4f836
ninja-win.zip https://github.com/ninja-build/ninja/releases/download/v$NINJA_VER/ninja-win.zip f550fec705b6d6ff58f2db3c374c2277a37691678d6aba463adcbb129108467a
python-$PYTHON_VER-embed-amd64.zip https://www.python.org/ftp/python/$PYTHON_VER/python-$PYTHON_VER-embed-amd64.zip 8d3f33be9eb810f23c102f08475af2854e50484b8e4e06275e937be61ce3d2fb
make-$MAKE_VER.tar.gz https://ftp.gnu.org/gnu/make/make-$MAKE_VER.tar.gz dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3
firmware/polaris10_rlc.bin $fw_url/amdgpu/polaris10_rlc.bin?h=$FIRMWARE_TAG a5d4c2d3acc5c3c4988b95855fe21a5f682552b2b065f76439370835178ccbaf
firmware/polaris10_mec.bin $fw_url/amdgpu/polaris10_mec.bin?h=$FIRMWARE_TAG c19c1e2e36b47c4a8c25d601f5ae04a26458d98c3000f851d1be5c3a0a424b07
firmware/polaris10_mec_2.bin $fw_url/amdgpu/polaris10_mec_2.bin?h=$FIRMWARE_TAG d4914f1166e3b7af212837aaf8f393a155ce3f61f48bb08279c743a2ddea0b6a
firmware/LICENSE.amdgpu $fw_url/LICENSES/LICENSE.amdgpu?h=$FIRMWARE_TAG 572872598565dc3513470de971a32bf9db301f47afeef3636345eadae33b2eee
"

fetch_all() {
    echo "$DEPS" | while read -r name url hash; do
        [ -n "$name" ] || continue
        f="$deps/$name"
        if [ -f "$f" ] && [ "$(sha256 "$f")" = "$hash" ]; then
            continue
        fi
        echo "downloading $name"
        mkdir -p "$(dirname "$f")"
        "$CURL" -fL --retry 3 -o "$(w "$f.part")" "$url" ||
            die "download of $url failed"
        got=$(sha256 "$f.part")
        [ "$got" = "$hash" ] ||
            die "$name: sha256 $got, expected $hash"
        mv "$f.part" "$f"
    done
}

# unpack <archive> <into dir> [members...], once
unpack() {
    a=$1 d=$2
    shift 2
    [ -e "$d/.unpacked" ] && return 0
    mkdir -p "$d"
    "$BSDTAR" -xf "$(w "$a")" -C "$(w "$d")" "$@"
    touch "$d/.unpacked"
}

say "downloads"
fetch_all

say "build tools"
tools="$work/tools"
unpack "$deps/cmake-$CMAKE_VER-windows-x86_64.zip" "$tools/cmake"
unpack "$deps/ninja-win.zip" "$tools/ninja"
unpack "$deps/python-$PYTHON_VER-embed-amd64.zip" "$tools/python"
CMAKE="$tools/cmake/cmake-$CMAKE_VER-windows-x86_64/bin/cmake.exe"
NINJA="$tools/ninja/ninja.exe"
PYTHON="$tools/python/python.exe"
"$CMAKE" --version | head -1
"$PYTHON" --version

say "LLVM $LLVM_VER source"
llvm_src="$work/src/llvm-project-$LLVM_VER.src"
# The patches are applied to a freshly unpacked tree; when they change, the
# tree is unpacked again.  Unpacked files keep their archive dates, so the
# build then only recompiles what the patches touch.
patches_id=$(cat "$here"/patches/llvm/*.patch | sha256sum 2>/dev/null |
             cut -d' ' -f1)
[ -n "$patches_id" ] || patches_id=$(ls -l "$here"/patches/llvm | cksum)
if [ "$(cat "$llvm_src/.gcn4xp-patches" 2>/dev/null)" != "$patches_id" ]; then
    rm -rf "$llvm_src"
    mkdir -p "$work/src"
    # llvm, clang, lld and lldb, without their (large) test suites
    "$BSDTAR" -xf "$(w "$deps/llvm-project-$LLVM_VER.src.tar.xz")" \
        -C "$(w "$work/src")" \
        --exclude '*/test/*' --exclude '*/unittests/*' \
        "llvm-project-$LLVM_VER.src/llvm" "llvm-project-$LLVM_VER.src/clang" \
        "llvm-project-$LLVM_VER.src/lld" "llvm-project-$LLVM_VER.src/lldb" \
        "llvm-project-$LLVM_VER.src/cmake" \
        "llvm-project-$LLVM_VER.src/third-party" \
        "llvm-project-$LLVM_VER.src/libunwind/include"
    for p in "$here"/patches/llvm/*.patch; do
        echo "applying $(basename "$p")"
        if command -v patch >/dev/null 2>&1; then
            (cd "$llvm_src" && patch -p1 --quiet < "$p")
        else
            (cd "$llvm_src" && git apply -p1 "$p")
        fi
    done
    echo "$patches_id" > "$llvm_src/.gcn4xp-patches"
fi

say "LLVM: clang, lld, lldb (this takes a while)"
llvm_build="$work/llvm"
if [ ! -x "$llvm_build/bin/lld.exe" ]; then
    # first lld, linked by GNU ld; it then links everything else
    CMAKE="$CMAKE" NINJA="$NINJA" PYTHON="$PYTHON" JOBS=$jobs \
        BUILD_TARGETS=lld sh "$here/build-llvm.sh" "$llvm_src" "$llvm_build"
    cp "$llvm_build/bin/lld.exe" "$work/ld.lld.exe"
fi
CMAKE="$CMAKE" NINJA="$NINJA" PYTHON="$PYTHON" JOBS=$jobs \
    LLD_LINKER="$work/ld.lld.exe" \
    sh "$here/build-llvm.sh" "$llvm_src" "$llvm_build"

say "GNU make $MAKE_VER"
unpack "$deps/make-$MAKE_VER.tar.gz" "$work/src/make"
make_exe="$work/make/make.exe"
# always rebuilt (seconds), so that it never misses a change of flags
sh "$here/build-make.sh" "$work/src/make/make-$MAKE_VER" "$work/make/obj" \
    "$make_exe"

say "tool alias launcher"
"$MINGW/bin/gcc.exe" -O2 $GCC_FIX -s -std=gnu99 -Wall -D_WIN32_WINNT=0x0501 -static \
    -o "$(w "$work/launcher.exe")" "$(w "$here/launcher.c")"

# ---- the SDK ------------------------------------------------------------------

say "staging $stage"
rm -rf "$work/stage"
mkdir -p "$stage/bin" "$stage/include" "$stage/lib/gcc/i686-w64-mingw32/$gccver" \
    "$stage/share/licenses" "$stage/firmware"

# LLVM programs, stripped
for t in clang lld lldb llvm-ar llvm-nm llvm-objcopy llvm-objdump \
         llvm-readobj; do
    "$MINGW/bin/strip.exe" --strip-unneeded -o "$stage/bin/$t.exe" \
        "$llvm_build/bin/$t.exe"
done
"$MINGW/bin/strip.exe" --strip-unneeded -o "$stage/bin/liblldb.dll" \
    "$llvm_build/bin/liblldb.dll"
# aliases: launchers where an option selects the personality, else copies
for a in clang++ clang-cl clang-cpp ld.lld lld-link llvm-readelf; do
    cp "$work/launcher.exe" "$stage/bin/$a.exe"
done
cp "$stage/bin/llvm-ar.exe" "$stage/bin/llvm-ranlib.exe"
cp "$stage/bin/llvm-objcopy.exe" "$stage/bin/llvm-strip.exe"
cp "$make_exe" "$stage/bin/make.exe"
# clang's own headers
mkdir -p "$stage/lib/clang"
cp -r "$llvm_build/lib/clang/${LLVM_VER%%.*}" "$stage/lib/clang/"
# defaults for XP programs
cp "$here/config/i686-w64-windows-gnu.cfg" "$stage/bin/"

# MinGW-w64 headers, start-up code, import libraries, static GCC runtime
(cd "$MINGW/include" && "$BSDTAR" -cf - \
    --exclude 'gmp*.h' --exclude 'mpfr*.h' --exclude 'mpf2mpfr.h' \
    --exclude 'mpc.h' --exclude 'isl' --exclude 'zstd*.h' --exclude 'zdict.h' \
    --exclude 'zlib.h' --exclude 'zconf.h' --exclude 'iconv.h' \
    --exclude 'libintl.h' --exclude 'libcharset.h' \
    --exclude 'localcharset.h' --exclude 'autosprintf.h' \
    --exclude 'textstyle*' .) | (cd "$stage/include" && "$BSDTAR" -xf -)
cp "$MINGW"/lib/*.o "$stage/lib/"
for a in "$MINGW"/lib/*.a; do
    n=$(basename "$a")
    case "$n" in
    # the compiler's own dependencies
    libgmp*|libmpfr*|libmpc*|libisl*|libzstd*|libz.*|libz_*|libiconv*|\
    libintl*|libcharset*|libgettext*|libasprintf*|libtextstyle*) continue ;;
    # Windows 8+ API-set umbrella libraries: never usable on XP
    libonecore*|libmincore*|libwindowsapp*|libwindowscoreheadless*) continue ;;
    # DLL import stubs of the runtimes: link them statically, always
    libstdc++.dll.a|libwinpthread.dll.a|libatomic.dll.a|libquadmath.dll.a|\
    libssp.dll.a|libgomp.dll.a|libgcc_s.a) continue ;;
    esac
    cp "$a" "$stage/lib/"
done
for f in libgcc.a libgcc_eh.a crtbegin.o crtend.o crtfastmath.o; do
    cp "$MINGW/lib/gcc/i686-w64-mingw32/$gccver/$f" \
        "$stage/lib/gcc/i686-w64-mingw32/$gccver/"
done

say "HSA runtime"
# built with the MinGW GCC; a static library clang links like its own
PATH="$MINGW/bin:$PATH" "$make_exe" -C "$repo/hsa" clean
PATH="$MINGW/bin:$PATH" "$make_exe" -C "$repo/hsa" -j "$jobs" install \
    PREFIX="$stage" GCC_FIX="$GCC_FIX"
cp "$deps"/firmware/*.bin "$stage/firmware/"

say "examples"
mkdir -p "$stage/examples"
(cd "$repo/hsa/examples" && "$BSDTAR" -cf - --exclude '*.exe' \
    --exclude '*.co' --exclude '*.o' .) |
    (cd "$stage/examples" && "$BSDTAR" -xf -)

say "licenses"
lic="$stage/share/licenses"
mkdir -p "$lic/llvm" "$lic/make" "$lic/firmware"
cp "$llvm_src/llvm/LICENSE.TXT" "$lic/llvm/"
cp "$work/src/make/make-$MAKE_VER/COPYING" "$lic/make/"
cp "$deps/firmware/LICENSE.amdgpu" "$lic/firmware/"
for l in crt headers winpthreads libwinpthread gcc-libs; do
    [ -d "$MINGW/share/licenses/$l" ] && cp -r "$MINGW/share/licenses/$l" "$lic/"
done
# text files for Notepad: CRLF line ends
sed 's/$/\r/' "$here/sdk-readme.txt" > "$stage/README.txt"
sed 's/$/\r/' > "$stage/VERSIONS.txt" <<EOF
gcn4xp SDK, built $(date -u +%Y-%m-%d) from gcn4xp $(git -C "$repo" describe --always --dirty 2>/dev/null || echo "(unknown revision)")

LLVM (clang, lld, lldb)  $LLVM_VER, patched for XP (gcn4xp sdk/patches/llvm)
MinGW-w64 runtime        from GCC $gccver
GNU make                 $MAKE_VER  (source: https://ftp.gnu.org/gnu/make/)
linux-firmware           $FIRMWARE_TAG (polaris10 RLC and MEC)
EOF

if [ -n "$XP_DLLS" ]; then
    say "checking the programs against $XP_DLLS"
    "$PYTHON" -I "$(w "$here/check-imports.py")" "$(w "$XP_DLLS")" \
        $(for f in "$stage"/bin/*.exe "$stage"/bin/*.dll; do w "$f"; done)
fi

say "checking for paths of this machine"
# Nothing in the SDK may name where it was built: no build directories, no
# user name.  (Compilers like to keep source paths in __FILE__ strings and
# version banners.)
"$PYTHON" -I "$(w "$here/check-paths.py")" "$(w "$stage")" "$(w "$repo")" \
    "$repo" "${USERPROFILE:-}" "$(w "${HOME:-/nonexistent}")" ||
    die "the files above contain paths of this machine"

say "archive"
rm -f "$out"
mkdir -p "$(dirname "$out")"
"$BSDTAR" -a -cf "$(w "$out")" -C "$(w "$work/stage")" gcn4xp_sdk
ls -l "$out"
echo "done: $out"
