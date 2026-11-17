// s_memrealtime.s - Spins for the number of real-time clock ticks passed as the argument,
// reading the GPU's real-time counter with s_memrealtime.
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

  //get start timestamp
  s_memrealtime   s[4:5]
  //read kernel argument
  s_load_dwordx2  s[2:3], s[0:1], 0x00
  s_waitcnt 0

  //compute finish time s[0:1]
  s_add_u32 s0, s2, s4
  s_addc_u32 s1, s3, s5


  //while( finish_time > get_current_time() ) {}
loop_start:
  //get current timestamp
  s_memrealtime s[4:5]
  s_waitcnt 0

  //we don't have s_cmp_*64 instruction so have to compare u32 parts
  //alternatively we can move data to vgpr and use v_cmp_lt_u64
  s_cmp_lt_u32 s5, s1
  s_cbranch_scc1 loop_start
  s_cmp_gt_u32 s5, s1
  s_cbranch_scc1 loop_end
  s_cmp_lt_u32 s4, s0
  s_cbranch_scc1 loop_start
loop_end:

  s_endpgm
.Lhello_world_end:
.size hello_world, .Lhello_world_end - hello_world

.rodata
.p2align 6
.amdhsa_kernel hello_world
  .amdhsa_user_sgpr_kernarg_segment_ptr 1
  .amdhsa_kernarg_size 8
  .amdhsa_next_free_vgpr 1
  .amdhsa_next_free_sgpr 8
  .amdhsa_reserve_flat_scratch 0
  .amdhsa_reserve_xnack_mask 0
.end_amdhsa_kernel
