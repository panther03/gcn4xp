#!/bin/sh
# Builds the example kernels into code objects (.co) with LLVM's AMDGPU
# backend.  Run it wherever an LLVM with the AMDGPU target is installed;
# the target machine only needs the resulting .co files.
#
#   build-kernels.sh <output dir> [<examples dir of LLVM-AMDGPU-Assembler-Extra>]
#
# LLVM_BIN selects the LLVM tools directory (default: from PATH).  The
# examples directory is needed only for the OpenCL example.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: build-kernels.sh <output dir> [<examples dir>]}
examples=$2
bin=${LLVM_BIN:+$LLVM_BIN/}
mkdir -p "$out"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
target="-target amdgcn-amd-amdhsa -mcpu=gfx803"

for s in "$here"/kernels/*.s; do
    n=$(basename "$s" .s)
    "${bin}clang" -x assembler $target -c "$s" -o "$tmp/$n.o"
    "${bin}ld.lld" -shared "$tmp/$n.o" -o "$out/$n.co"
    echo "$out/$n.co"
done

if [ -n "$examples" ]; then
    cl="$examples/gfx8/s_memrealtime_inline.cl"
    "${bin}clang" -x cl -cl-std=CL1.2 $target -nogpulib -O2 -c "$cl" \
        -o "$tmp/s_memrealtime_inline.o"
    "${bin}ld.lld" -shared "$tmp/s_memrealtime_inline.o" \
        -o "$out/s_memrealtime_inline.co"
    echo "$out/s_memrealtime_inline.co"
fi
