# Examples

Small GPU programs for the gcn4xp HSA runtime. Each one is a host program
(`NAME.cpp`) that loads a GPU code object (`NAME.co`, built from `NAME.s`
or `NAME.cl`), runs the kernel once and checks the result. They are built
with the gcn4xp SDK on the XP machine:

```
set GCN4XP_SDK_HOME=C:\gcn4xp_sdk
cd %GCN4XP_SDK_HOME%\examples
make              all of them (or make inside one folder)
make run          run them; needs the GPU and the gcn4xp debug driver
make clean
```

| Example | Kernel | Shows |
|---------|--------|-------|
| `asm-kernel` | assembly | The smallest kernel: one store to a buffer passed as an argument. |
| `dpp_reduce` | assembly | A running sum across a wavefront with DPP (data-parallel primitive) row shifts and broadcasts. |
| `ds_bpermute` | assembly | Exchanging values between lanes with `ds_bpermute_b32`. |
| `fp16_native` | assembly | Native half-precision arithmetic (`v_add_f16`, new in gfx8). |
| `fp16_storage` | assembly | Half-precision storage with single-precision arithmetic. |
| `s_memrealtime` | assembly | Reading the GPU's real-time counter: the kernel spins for about one second. |
| `s_memrealtime_inline` | OpenCL C | The same with inline assembly in OpenCL C, compiled by clang. |

`common/` holds the host-side helper all of them use (`dispatch.cpp`:
agent and memory region discovery, code object loading, AQL dispatch) and
`example.mk`, the shared build rules.

## Credit

The examples come from AMD's AMDGPU assembler examples, Copyright (c) 2016
Advanced Micro Devices, Inc., under the University of Illinois/NCSA Open
Source License (`LICENSE.AMD`). Changes for gcn4xp:

- The kernels are ported from the code object v2 directives
  (`.hsa_code_object_version`, `.amd_kernel_code_t`), which current LLVM no
  longer accepts, to `.amdhsa_kernel`. The kernel bodies are unchanged.
- `common/dispatch.cpp` works in a 32-bit MinGW process. Its header lists
  the changes.
- The CMake build is replaced by the makefiles.
