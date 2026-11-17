// dpp_reduce.s - Running sum of a 64-element float array within one wavefront, using
// data-parallel primitives (DPP row shifts and broadcasts).
//
// From AMD's AMDGPU assembler examples, Copyright (c) 2016 Advanced Micro
// Devices, Inc., University of Illinois/NCSA Open Source License (see
// ../LICENSE.AMD).  Ported from code object v2 directives to .amdhsa_kernel;
// the kernel body is unchanged.
.amdgcn_target "amdgcn-amd-amdhsa--gfx803"
.text
.globl hello_world
.p2align 8
.type hello_world,@function
hello_world:

  s_load_dwordx4  s[0:3], s[0:1], 0x00
  v_lshlrev_b32  v0, 2, v0
  s_waitcnt     lgkmcnt(0)

  v_add_u32     v3, vcc, s2, v0
  v_mov_b32     v4, s3
  v_addc_u32    v4, vcc, v4, 0, vcc

  v_add_u32     v1, vcc, s0, v0
  v_mov_b32     v2, s1
  v_addc_u32    v2, vcc, v2, 0, vcc

  flat_load_dword  v0, v[1:2]
  s_waitcnt     vmcnt(0) & lgkmcnt(0)

  v_add_f32 v1, v0, v0 row_shr:1 bound_ctrl:0
  v_add_f32 v1, v0, v1 row_shr:2 bound_ctrl:0
  v_add_f32 v1, v0, v1 row_shr:3 bound_ctrl:0
  s_nop 0 // Nop required for data hazard in SP
  s_nop 0 // Nop required for data hazard in SP
  v_add_f32 v1, v1, v1 row_shr:4 bank_mask:0xe
  s_nop 0 // Nop required for data hazard in SP
  s_nop 0 // Nop required for data hazard in SP
  v_add_f32 v1, v1, v1 row_shr:8 bank_mask:0xc
  s_nop 0 // Nop required for data hazard in SP
  s_nop 0 // Nop required for data hazard in SP
  v_add_f32 v1, v1, v1 row_bcast:15 row_mask:0xa
  s_nop 0 // Nop required for data hazard in SP
  s_nop 0 // Nop required for data hazard in SP
  v_add_f32 v1, v1, v1 row_bcast:31 row_mask:0xc

  flat_store_dword v[3:4], v1
  s_endpgm
.Lhello_world_end:
.size hello_world, .Lhello_world_end - hello_world

.rodata
.p2align 6
.amdhsa_kernel hello_world
  .amdhsa_user_sgpr_kernarg_segment_ptr 1
  .amdhsa_kernarg_size 16
  .amdhsa_next_free_vgpr 5
  .amdhsa_next_free_sgpr 6
  .amdhsa_reserve_flat_scratch 0
  .amdhsa_reserve_xnack_mask 0
.end_amdhsa_kernel
