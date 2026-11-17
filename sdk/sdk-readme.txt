gcn4xp SDK
==========

Tools and a runtime for writing GPU compute programs for AMD Polaris cards
(RX 460-590) on Windows XP, with the gcn4xp display driver (debug build).

Setup
-----

Extract the archive anywhere without spaces in the path, for example to
C:\gcn4xp_sdk, then set two environment variables (System Properties >
Advanced > Environment Variables):

    GCN4XP_SDK_HOME = C:\gcn4xp_sdk
    PATH            = C:\gcn4xp_sdk\bin;...

Contents
--------

bin\        clang, clang++ (C/C++ for XP, and AMDGPU code)
            ld.lld, lld-link, llvm-ar, llvm-objdump, llvm-readelf, ...
            lldb (debugger), make (GNU make)
            gcngpu (GPU engine status/reset), hsatest (runtime self-test)
include\    Windows (MinGW-w64) and C++ headers; include\hsa\ is the HSA API
lib\        Windows import libraries, the C++ runtime, libhsa-runtime.a
firmware\   GPU microcode the runtime loads (from linux-firmware)
examples\   example GPU programs with makefiles
share\licenses\   licenses of the parts

Building programs
-----------------

clang builds 32-bit programs for XP by default: it links the C/C++ runtime
statically, so the .exe files need nothing installed.

    clang -O2 hello.c -o hello.exe
    clang++ -O2 -std=c++17 app.cpp -o app.exe

GPU programs use the HSA runtime (HSA Runtime 1.2 API):

    #include <hsa/hsa.h>
    clang++ -O2 app.cpp -o app.exe -lhsa-runtime -lgdi32 -luser32 -ladvapi32

GPU code (kernels) is compiled for the Polaris instruction set, gfx803,
into a code object that the program loads at run time:

    clang --target=amdgcn-amd-amdhsa -mcpu=gfx803 -nogpulib kernel.s -o kernel.co
    clang --target=amdgcn-amd-amdhsa -mcpu=gfx803 -nogpulib -cl-std=CL1.2 -O2 kernel.cl -o kernel.co

The tools are Windows programs: give them Windows paths (C:\... or C:/...).

Examples
--------

    cd %GCN4XP_SDK_HOME%\examples
    make            (builds them all; or "make" inside one example's folder)
    make run        (runs them; needs the GPU and the debug driver)
    make clean

See examples\README.md for what each example shows.

Debugging
---------

    lldb app.exe
    (lldb) breakpoint set --name main
    (lldb) run

Compile with -g for source-level debugging.

Runtime notes
-------------

- One program at a time can use the GPU.
- The runtime finds the firmware through GCN4XP_SDK_HOME; GCN4XP_FIRMWARE
  overrides that.  GCN4XP_TRACE=1 logs what it does.
- gcngpu status shows the engine state; gcngpu init -f resets it after a
  hang.
