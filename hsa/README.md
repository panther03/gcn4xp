# gcn4xp compute: an HSA runtime for Polaris on Windows XP

A small implementation of the HSA Runtime 1.2 API that loads AMDGPU code
objects (HSACO) and runs their kernels on the GPU, from 32-bit Windows XP.
It drives the hardware from user mode through the gcn4xp display driver,
so it needs a **debug build** of the driver (`build.ps1 -DebugBuild`,
version 0.1.1 or later).

Status: runs the gfx8 [examples](examples/README.md) on an RX 580.

Programs are written and built on XP with the gcn4xp SDK
([../sdk](../sdk/README.md)): clang for the host and GPU code, this
runtime, and the examples.

## How it works

The firmware leaves the shader engine cold. `src/gcn_gpu.c` loads the
RLC and compute (MEC) microcode, starts them, and programs one hardware
compute queue. The MEC microcode runs from video memory.

| Piece | What it does |
|-------|--------------|
| Memory | The compute half of the PCI aperture is mapped into the process. The GPU's user address space (VMID 1) maps the same virtual addresses to the same pages, so CPU and kernels share pointers. Every region allocates from that heap. |
| Queues | AQL queues live in host memory. Ringing the doorbell, or waiting on or loading any signal, runs a packet processor in the calling thread. It turns each kernel dispatch packet into a PM4 indirect buffer and tracks completion through a fence the GPU writes. |
| Coherence | Before each dispatch the GPU's L1/L2/K$/I$ are written back and invalidated, since the CPU writes through the BAR, past those caches. After each dispatch caches are flushed and the HDP read cache is dropped before signals complete. |
| Loader | `src/hsa_loader.c` takes ELF code objects v3–v6 for gfx803, relocates them into GPU memory, and finds kernels by their `name.kd` descriptors. |

Only one process can use the GPU at a time; `hsa_init` waits up to 10 s
for the lock.

## Building

The SDK script builds the runtime as part of the SDK. On its own, with a
32-bit MinGW-w64 GCC (`msvcrt`, e.g. MSYS2's `mingw32`) and GNU make:

```
make                        build/libhsa-runtime.a, build/gcngpu.exe, build/hsatest.exe
make install PREFIX=<dir>   <dir>/lib, <dir>/include/hsa, <dir>/bin
```

Programs include `<hsa/hsa.h>` (and `<hsa/hsa_ext_amd.h>`) and link with
`-lhsa-runtime -lgdi32 -luser32 -ladvapi32`. The library is plain C built
by GCC; clang and lld link it like their own. With GCC 15 the Makefile adds
`-fno-late-combine-instructions`, which works around a miscompile (see
[../sdk/gcc-check.c](../sdk/gcc-check.c)).

### Firmware

The runtime needs the Polaris10 RLC and MEC images from linux-firmware
(`polaris10_rlc.bin`, `polaris10_mec_2.bin` or `polaris10_mec.bin`). It
looks in `GCN4XP_FIRMWARE`; then in `firmware\` next to the program or next
to its parent directory; then in `%GCN4XP_SDK_HOME%\firmware`. The SDK
includes them; this repository does not.

## Tools

```
gcngpu init [-f]     start the engine (-f: reset it first)
gcngpu status        engine and queue registers
gcngpu cstest        one-lane shader without address translation
gcngpu vmtest        the same through the shared user address space
gcngpu memtest       CPU reads of video memory at each access width
hsatest <file.co> [kernel]   load and run a kernel via the HSA API
```

`GCN4XP_TRACE=1` makes the runtime log bring-up, loaded kernels and each
dispatch to stderr.

## Not implemented

- Private (scratch) memory: kernels that use it are rejected at dispatch.
- Code object v2, images, samplers, the finalizer, profiling, agent
  dispatch packets, soft queues, program-allocated global variables.
- Interrupts: waiting polls the fence.
- GPU hang recovery inside a program. `gcngpu init -f` resets the engine.
