/*
 * gcn_regs.h - graphics/compute engine registers and PM4 packet encodings
 * for GCN 3/4 ("VI") parts.  Register numbers are dword indices into the
 * MMIO BAR and follow the Linux amdgpu register headers.
 */
#ifndef GCN_REGS_H
#define GCN_REGS_H

/* ---- bus interface / memory controller --------------------------------- */

#define mmMM_INDEX                      0x0000
#define mmMM_DATA                       0x0001
#define mmMM_INDEX_HI                   0x0006

#define mmSRBM_GFX_CNTL                 0x0391 /* VMID in bits 4..7 */
#define mmSRBM_STATUS                   0x0394

#define mmVM_L2_CNTL                    0x0500
#define mmVM_L2_CNTL2                   0x0501
#define mmVM_L2_CNTL3                   0x0502
#define mmVM_CONTEXT0_CNTL              0x0504
#define mmVM_CONTEXT1_CNTL              0x0505
#define mmVM_CONTEXT0_CNTL2             0x050c
#define mmVM_CONTEXT1_CNTL2             0x050d
#define mmVM_INVALIDATE_REQUEST         0x051e
#define mmVM_INVALIDATE_RESPONSE        0x051f
#define mmVM_CONTEXT0_PROTECTION_FAULT_STATUS 0x0536
#define mmVM_CONTEXT1_PROTECTION_FAULT_STATUS 0x0537
#define mmVM_CONTEXT1_PROTECTION_FAULT_MCCLIENT 0x0539
#define mmVM_CONTEXT0_PROTECTION_FAULT_ADDR 0x053e
#define mmVM_CONTEXT1_PROTECTION_FAULT_ADDR 0x053f
#define mmVM_CONTEXT0_PROTECTION_FAULT_DEFAULT_ADDR 0x0546
#define mmVM_CONTEXT1_PROTECTION_FAULT_DEFAULT_ADDR 0x0547
#define mmVM_CONTEXT0_PAGE_TABLE_BASE_ADDR 0x054f /* +n for contexts 0..7 */
#define mmVM_CONTEXT0_PAGE_TABLE_START_ADDR 0x0557
#define mmVM_CONTEXT1_PAGE_TABLE_START_ADDR 0x0558 /* shared by 1..15 */
#define mmVM_CONTEXT0_PAGE_TABLE_END_ADDR 0x055f
#define mmVM_CONTEXT1_PAGE_TABLE_END_ADDR 0x0560
#define mmVM_L2_CNTL4                   0x0578

#define mmMC_VM_FB_LOCATION             0x0809
#define mmMC_VM_SYSTEM_APERTURE_LOW_ADDR 0x080d
#define mmMC_VM_SYSTEM_APERTURE_HIGH_ADDR 0x080e
#define mmMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR 0x080f
#define mmMC_VM_MX_L1_TLB_CNTL          0x0819

#define mmHDP_MEM_COHERENCY_FLUSH_CNTL  0x1520
#define mmHDP_DEBUG0                    0x0bcc  /* write 1: drop HDP read cache */

/* ---- GRBM / CP --------------------------------------------------------- */

#define mmGRBM_CNTL                     0x2000
#define mmGRBM_STATUS2                  0x2002
#define mmGRBM_STATUS                   0x2004
#define mmGRBM_SOFT_RESET               0x2008
#define   GRBM_SOFT_RESET_CP              0x00000001
#define   GRBM_SOFT_RESET_RLC             0x00000004
#define   GRBM_SOFT_RESET_GFX             0x00010000
#define   GRBM_SOFT_RESET_CPF             0x00020000
#define   GRBM_SOFT_RESET_CPC             0x00040000
#define   GRBM_SOFT_RESET_CPG             0x00080000
#define mmGMCON_DEBUG                   0x0d5f
#define   GMCON_DEBUG_GFX_STALL           0x00000001
#define   GMCON_DEBUG_GFX_CLEAR           0x00000002
#define mmGRBM_GFX_INDEX                0xc200

#define mmCP_CPC_STATUS                 0x2084
#define mmCP_CPF_STATUS                 0x2087
#define mmCP_MEC_CNTL                   0x208d
#define mmCP_STALLED_STAT3              0x219c
#define mmCP_STALLED_STAT1              0x219d
#define mmCP_STALLED_STAT2              0x219e
#define mmCP_BUSY_STAT                  0x219f
#define mmCP_STAT                       0x21a0
#define mmCP_ME_HEADER_DUMP             0x21a1
#define mmCP_PFP_HEADER_DUMP            0x21a2
#define mmCP_ME_CNTL                    0x21b6
#define   CP_ME_CNTL_CE_HALT              0x01000000
#define   CP_ME_CNTL_PFP_HALT             0x04000000
#define   CP_ME_CNTL_ME_HALT              0x10000000
#define mmCP_RB0_RPTR                   0x21c0
#define mmCP_RB_WPTR_DELAY              0x21c1

#define mmCP_RB0_BASE                   0x3040
#define mmCP_RB0_CNTL                   0x3041
#define   CP_RB0_CNTL_RB_RPTR_WR_ENA      0x80000000
#define mmCP_RB0_RPTR_ADDR              0x3043
#define mmCP_RB0_RPTR_ADDR_HI           0x3044
#define mmCP_RB0_WPTR                   0x3045
#define mmCP_DEVICE_ID                  0x304b
#define mmCP_ENDIAN_SWAP                0x3050
#define mmCP_RB_VMID                    0x3051
#define mmCP_RB_DOORBELL_CONTROL        0x3059
#define mmCP_INT_CNTL_RING0             0x306a
#define mmCP_MAX_CONTEXT                0x30ae
#define mmCP_RB0_BASE_HI                0x30b1

#define mmCP_CPC_STATUS_                0x2084
#define mmCP_CPC_BUSY_STAT              0x2085
#define mmCP_CPC_STALLED_STAT1          0x2086
#define mmCP_CPF_BUSY_STAT              0x2088
#define mmCP_CPF_STALLED_STAT1          0x2089
#define mmCP_MEC_ME1_HEADER_DUMP        0x208e
#define   CP_MEC_ME2_HALT                 0x10000000
#define   CP_MEC_ME1_HALT                 0x40000000
#define mmCP_PQ_WPTR_POLL_CNTL          0x3083
#define   CP_PQ_WPTR_POLL_EN              0x80000000
#define mmCP_PQ_STATUS                  0x30b8
#define mmCP_CPC_IC_BASE_LO             0x30b9
#define mmCP_CPC_IC_BASE_HI             0x30ba
#define mmCP_CPC_IC_BASE_CNTL           0x30bb
#define   CP_MEC_INVALIDATE_ICACHE        0x00000010

/* hardware queue descriptor of the queue selected in SRBM_GFX_CNTL */
#define mmCP_MQD_BASE_ADDR              0x3245
#define mmCP_MQD_BASE_ADDR_HI           0x3246
#define mmCP_HQD_ACTIVE                 0x3247
#define mmCP_HQD_VMID                   0x3248
#define mmCP_HQD_PERSISTENT_STATE       0x3249
#define mmCP_HQD_PQ_BASE                0x324d
#define mmCP_HQD_PQ_BASE_HI             0x324e
#define mmCP_HQD_PQ_RPTR                0x324f
#define mmCP_HQD_PQ_RPTR_REPORT_ADDR    0x3250
#define mmCP_HQD_PQ_RPTR_REPORT_ADDR_HI 0x3251
#define mmCP_HQD_PQ_WPTR_POLL_ADDR      0x3252
#define mmCP_HQD_PQ_WPTR_POLL_ADDR_HI   0x3253
#define mmCP_HQD_PQ_DOORBELL_CONTROL    0x3254
#define mmCP_HQD_PQ_WPTR                0x3255
#define mmCP_HQD_PQ_CONTROL             0x3256
#define   HQD_PQ_MTYPE(x)                 ((u32)(x) << 15)
#define   HQD_PQ_MIN_AVAIL_SIZE(x)        ((u32)(x) << 20)
#define   HQD_PQ_NO_UPDATE_RPTR           0x08000000
#define   HQD_PQ_PRIV_STATE               0x40000000
#define   HQD_PQ_KMD_QUEUE                0x80000000
#define mmCP_HQD_IB_BASE_ADDR           0x3257
#define mmCP_HQD_IB_BASE_ADDR_HI        0x3258
#define mmCP_HQD_IB_RPTR                0x3259
#define mmCP_HQD_IB_CONTROL             0x325a
#define   HQD_IB_MIN_AVAIL_SIZE(x)        ((u32)(x) << 20)
#define   HQD_IB_MTYPE(x)                 ((u32)(x) << 27)
#define   HQD_MQD_MTYPE(x)                ((u32)(x) << 27)
#define mmCP_HQD_IQ_RPTR                0x325c
#define mmCP_HQD_DEQUEUE_REQUEST        0x325d
#define mmCP_MQD_CONTROL                0x3267
#define mmCP_HQD_EOP_BASE_ADDR          0x326a
#define mmCP_HQD_EOP_BASE_ADDR_HI       0x326b
#define mmCP_HQD_EOP_CONTROL            0x326c
#define   HQD_EOP_MTYPE(x)                ((u32)(x) << 15)
#define mmCP_HQD_EOP_RPTR               0x326d
#define mmCP_HQD_EOP_WPTR               0x326e
#define mmCP_HQD_EOP_EVENTS             0x326f
#define mmCP_HQD_ERROR                  0x3278
#define mmCP_HQD_EOP_WPTR_MEM           0x3279
#define mmCP_HQD_IQ_TIMER               0x325b

#define mmCP_MEC_ME1_UCODE_ADDR         0xf81a
#define mmCP_MEC_ME1_UCODE_DATA         0xf81b
#define mmCP_PFP_UCODE_ADDR             0xf814
#define mmCP_PFP_UCODE_DATA             0xf815
#define mmCP_ME_RAM_WADDR               0xf816
#define mmCP_ME_RAM_DATA                0xf817
#define mmCP_CE_UCODE_ADDR              0xf818
#define mmCP_CE_UCODE_DATA              0xf819

#define mmSCRATCH_REG0                  0xc040
#define mmSCRATCH_REG1                  0xc041
#define mmSCRATCH_REG2                  0xc042
#define mmCP_IB1_BASE_LO                0xc0cc
#define mmCP_IB1_BASE_HI                0xc0cd
#define mmCP_IB1_BUFSZ                  0xc0ce

/* ---- RLC --------------------------------------------------------------- */

#define mmRLC_CNTL                      0xec00
#define mmRLC_STAT                      0xec04
#define mmRLC_GPM_STAT                  0xec40
#define mmRLC_PG_CNTL                   0xec43
#define mmRLC_CGCG_CGLS_CTRL            0xec49
#define mmRLC_SERDES_CU_MASTER_BUSY     0xec61
#define mmRLC_SERDES_NONCU_MASTER_BUSY  0xec62
#define mmRLC_CGCG_CGLS_CTRL_3D         0xec9d
#define mmRLC_GPM_UCODE_ADDR            0xf83c
#define mmRLC_GPM_UCODE_DATA            0xf83d

/* ---- shader core ------------------------------------------------------- */

#define mmSQ_CONFIG                     0x2300
#define mmSH_MEM_BASES                  0x230a
#define mmSH_MEM_APE1_BASE              0x230b
#define mmSH_MEM_APE1_LIMIT             0x230c
#define mmSH_MEM_CONFIG                 0x230d
#define   SH_MEM_CONFIG_ALIGNMENT_UNALIGNED (3u << 3)
#define   SH_MEM_CONFIG_DEFAULT_MTYPE(x)  ((x) << 5)
#define   SH_MEM_CONFIG_APE1_MTYPE(x)     ((x) << 8)
#define   MTYPE_NC                        0
#define   MTYPE_CC                        2
#define   MTYPE_UC                        3
#define mmGB_ADDR_CONFIG                0x263e

#define mmCOMPUTE_DISPATCH_INITIATOR    0x2e00
#define   COMPUTE_SHADER_EN               0x00000001
#define   PARTIAL_TG_EN                   0x00000002
#define   FORCE_START_AT_000              0x00000004
#define   USE_THREAD_DIMENSIONS           0x00000020
#define mmCOMPUTE_START_X               0x2e04
#define mmCOMPUTE_NUM_THREAD_X          0x2e07
#define mmCOMPUTE_PERFCOUNT_ENABLE      0x2e0b
#define mmCOMPUTE_PGM_LO                0x2e0c
#define mmCOMPUTE_PGM_HI                0x2e0d
#define mmCOMPUTE_TBA_LO                0x2e0e
#define mmCOMPUTE_PGM_RSRC1             0x2e12
#define mmCOMPUTE_PGM_RSRC2             0x2e13
#define mmCOMPUTE_VMID                  0x2e14
#define mmCOMPUTE_RESOURCE_LIMITS       0x2e15
#define mmCOMPUTE_STATIC_THREAD_MGMT_SE0 0x2e16
#define mmCOMPUTE_STATIC_THREAD_MGMT_SE1 0x2e17
#define mmCOMPUTE_TMPRING_SIZE          0x2e18
#define mmCOMPUTE_STATIC_THREAD_MGMT_SE2 0x2e19
#define mmCOMPUTE_STATIC_THREAD_MGMT_SE3 0x2e1a
#define mmCOMPUTE_THREAD_TRACE_ENABLE   0x2e1e
#define mmCOMPUTE_MISC_RESERVED         0x2e1f
#define mmCOMPUTE_USER_DATA_0           0x2e40

/* COMPUTE_PGM_RSRC2 fields */
#define   RSRC2_SCRATCH_EN                0x00000001
#define   RSRC2_USER_SGPR(x)              ((x) << 1)
#define   RSRC2_USER_SGPR_MASK            (0x1fu << 1)
#define   RSRC2_TGID_X_EN                 (1u << 7)
#define   RSRC2_TGID_Y_EN                 (1u << 8)
#define   RSRC2_TGID_Z_EN                 (1u << 9)
#define   RSRC2_TG_SIZE_EN                (1u << 10)
#define   RSRC2_TIDIG_COMP_CNT(x)         ((x) << 11)
#define   RSRC2_TIDIG_COMP_CNT_MASK       (3u << 11)
#define   RSRC2_LDS_SIZE(x)               ((x) << 15)
#define   RSRC2_LDS_SIZE_MASK             (0x1ffu << 15)

/* ---- PM4 --------------------------------------------------------------- */

#define PACKET3(op, n) \
    ((3u << 30) | (((u32)(op) & 0xff) << 8) | (((u32)(n) & 0x3fff) << 16))
#define PACKET3_COMPUTE(op, n) (PACKET3(op, n) | 2u)
#define PACKET3_NOP_DWORD               0xffff1000u /* one-dword filler */

#define PACKET3_NOP                     0x10
#define PACKET3_SET_BASE                0x11
#define   CE_PARTITION_BASE               3
#define PACKET3_CLEAR_STATE             0x12
#define PACKET3_DISPATCH_DIRECT         0x15
#define PACKET3_CONTEXT_CONTROL         0x28
#define PACKET3_WRITE_DATA              0x37
#define PACKET3_INDIRECT_BUFFER         0x3f
#define   INDIRECT_BUFFER_VALID           (1u << 23)
#define PACKET3_RELEASE_MEM             0x49
#define   EOP_TC_WB_ACTION_EN             (1u << 15)
#define   EOP_TCL1_ACTION_EN              (1u << 16)
#define   EOP_TC_ACTION_EN                (1u << 17)
#define   CACHE_FLUSH_AND_INV_TS_EVENT    0x14
#define   DATA_SEL(x)                     ((u32)(x) << 29)
#define   INT_SEL(x)                      ((u32)(x) << 24)
#define PACKET3_PFP_SYNC_ME             0x42
#define PACKET3_EVENT_WRITE             0x46
#define   EVENT_TYPE(x)                   ((x) << 0)
#define   EVENT_INDEX(x)                  ((x) << 8)
#define   CS_PARTIAL_FLUSH                7
#define PACKET3_EVENT_WRITE_EOP         0x47
#define PACKET3_PREAMBLE_CNTL           0x4a
#define   PREAMBLE_BEGIN_CLEAR_STATE      (2u << 28)
#define   PREAMBLE_END_CLEAR_STATE        (3u << 28)
#define PACKET3_ACQUIRE_MEM             0x58
/* CP_COHER_CNTL bits of ACQUIRE_MEM */
#define   COHER_TC_WB_ACTION_ENA          (1u << 18)
#define   COHER_TCL1_ACTION_ENA           (1u << 22)
#define   COHER_TC_ACTION_ENA             (1u << 23)
#define   COHER_SH_KCACHE_ACTION_ENA      (1u << 27)
#define   COHER_SH_ICACHE_ACTION_ENA      (1u << 29)
#define PACKET3_SET_CONFIG_REG          0x68
#define   SET_CONFIG_REG_START            0x2000
#define PACKET3_SET_CONTEXT_REG         0x69
#define   SET_CONTEXT_REG_START           0xa000
#define PACKET3_SET_SH_REG              0x76
#define   SET_SH_REG_START                0x2c00
#define PACKET3_SET_UCONFIG_REG         0x79
#define   SET_UCONFIG_REG_START           0xc000

#endif
