// Ported from LLVM-AMDGPU-Assembler-Extra examples/asm-kernel/asm-kernel.s
// (University of Illinois/NCSA Open Source License, Copyright (c) 2016
// Advanced Micro Devices, Inc.) to code object v5 directives.
// Stores 3.14159 at the pointer passed as the only kernel argument.
.amdgcn_target "amdgcn-amd-amdhsa--gfx803"
.text
.globl hello_world
.p2align 8
.type hello_world,@function
hello_world:
  s_load_dwordx2 s[0:1], s[0:1] 0x0
  v_mov_b32 v0, 3.14159
  s_waitcnt lgkmcnt(0)
  v_mov_b32 v1, s0
  v_mov_b32 v2, s1
  flat_store_dword v[1:2], v0
  s_waitcnt 0
  s_endpgm
.Lhello_world_end:
.size hello_world, .Lhello_world_end - hello_world

.rodata
.p2align 6
.amdhsa_kernel hello_world
  .amdhsa_user_sgpr_kernarg_segment_ptr 1
  .amdhsa_kernarg_size 8
  .amdhsa_next_free_vgpr 3
  .amdhsa_next_free_sgpr 2
  .amdhsa_reserve_flat_scratch 0
  .amdhsa_reserve_xnack_mask 0
.end_amdhsa_kernel
