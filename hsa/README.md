# gcn4xp compute: an HSA runtime for Polaris on Windows XP

A small implementation of the HSA Runtime 1.2 API that loads AMDGPU code
objects (HSACO) and runs their kernels on the GPU, from 32-bit Windows XP.
It drives the hardware from user mode through the gcn4xp display driver,
so it needs a **debug build** of the driver (`build.ps1 -DebugBuild`,
version 0.1.1 or later).

Status: runs the gfx8 examples of
[LLVM-AMDGPU-Assembler-Extra](https://github.com/ROCm/LLVM-AMDGPU-Assembler-Extra)
(`asm-kernel`, `dpp_reduce`, `ds_bpermute`, `fp16_native`, `fp16_storage`,
`s_memrealtime`, `s_memrealtime_inline`) on an RX 580.

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

## Building (on the XP machine, Cygwin)

```
make                       # build/libhsa-runtime.a, build/gcngpu.exe, build/hsatest.exe
make examples EXAMPLES=<path>/LLVM-AMDGPU-Assembler-Extra/examples
```

Programs link with `build/libhsa-runtime.a -lgdi32 -luser32 -ladvapi32`
and include `include/hsa.h` (and `hsa_ext_amd.h`).

The example host code assumes a 64-bit process when it passes buffer
pointers to kernels. The `examples` target compiles a copy of
`common/dispatch.cpp` with that one line changed; the checkout itself is
not modified.

### Firmware

The runtime needs the Polaris10 RLC and MEC images from linux-firmware
(`polaris10_rlc.bin`, `polaris10_mec_2.bin` or `polaris10_mec.bin`; the
`amdgpu/` directory). It looks in `GCN4XP_FIRMWARE`, else in `firmware\`
next to the program or next to its parent directory. The images are not
part of this repository.

## Kernels

GPU code is built elsewhere with any LLVM that has the AMDGPU backend:

```
LLVM_BIN=<llvm>/bin sh examples/build-kernels.sh <out dir> [<examples dir>]
```

`examples/kernels/` holds the gfx8 assembler examples ported from the old
`.hsa_code_object_version` / `.amd_kernel_code_t` directives (code object
v2, which current LLVM no longer accepts) to `.amdhsa_kernel`. The kernel
bodies are unchanged. Copy the `.co` files next to the example programs.

## Tools

```
gcngpu init [-f]     start the engine (-f: reset it first)
gcngpu status        engine and queue registers
gcngpu cstest        one-lane shader without address translation
gcngpu vmtest        the same through the shared user address space
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
