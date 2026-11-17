// Ported from LLVM-AMDGPU-Assembler-Extra examples/gfx8/fp16_storage.s
// (University of Illinois/NCSA Open Source License, Copyright (c) 2016
// Advanced Micro Devices, Inc.) to code object v5+ directives; the
// kernel body is unchanged.
.amdgcn_target "amdgcn-amd-amdhsa--gfx803"
.text
.globl hello_world
.p2align 8
.type hello_world,@function
hello_world:

  // read kernel arguments:
  // s[0:1] = half *in1
  // s[2:3] = half *in2
  // s[4:5] = half *out
  s_load_dwordx2 s[4:5], s[0:1], 0x10
  s_load_dwordx4 s[0:3], s[0:1], 0x00
  
  v_lshlrev_b32 v0, 1, v0
  s_waitcnt     0
  
  // v[1:2] = &in1[i]
  v_add_u32     v1, vcc, s0, v0
  v_mov_b32     v2, s1
  v_addc_u32    v2, vcc, v2, 0, vcc
  flat_load_ushort v3, v[1:2] // v3 = in1[i]
  
  // v[1:2] = &in2[i]
  v_add_u32     v1, vcc, s2, v0
  v_mov_b32     v2, s3
  v_addc_u32    v2, vcc, v2, 0, vcc  
  flat_load_ushort v2, v[1:2] // v2 = in2[i]
  
  // v[0:1] = &out[i]
  v_add_u32     v0, vcc, s4, v0
  v_mov_b32     v1, s5
  v_addc_u32    v1, vcc, v1, 0, vcc
  
  // wait for memory operations to complete
  s_waitcnt     0
  
  // convert input data to f32
  v_cvt_f32_f16 v3, v3
  v_cvt_f32_f16 v2, v2
  
  v_add_f32     v3, v3, v2 // v3 = in1[i] + in2[i]
  
  //convert result back to f16
  v_cvt_f16_f32 v3, v3

  flat_store_short v[0:1], v3 // out[i] = v3
  s_endpgm
.Lhello_world_end:
.size hello_world, .Lhello_world_end - hello_world

.rodata
.p2align 6
.amdhsa_kernel hello_world
  .amdhsa_user_sgpr_kernarg_segment_ptr 1
  .amdhsa_kernarg_size 24
  .amdhsa_next_free_vgpr 4
  .amdhsa_next_free_sgpr 8
  .amdhsa_reserve_flat_scratch 0
  .amdhsa_reserve_xnack_mask 0
.end_amdhsa_kernel
