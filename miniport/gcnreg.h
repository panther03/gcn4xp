/*
 * gcnreg.h - GPU register numbers used by the miniport.
 *
 * Values are register (dword) indices into the MMIO BAR, as published in
 * the Linux amdgpu register headers (dce_11_2_d.h, gmc_8_1_d.h,
 * bif_5_1_d.h).  Byte offset = index * 4.
 */
#ifndef GCN_REG_H
#define GCN_REG_H

/* bus interface */
#define mmCONFIG_MEMSIZE                    0x150a /* VRAM size in MB */

/* memory controller */
#define mmMC_VM_FB_LOCATION                 0x0809 /* [15:0] base>>24, [31:16] top>>24 */

/* display controller, CRTC 0 instance; add the per-CRTC offset */
#define mmGRPH_ENABLE                       0x1a00
#define mmGRPH_CONTROL                      0x1a01
#define mmGRPH_PRIMARY_SURFACE_ADDRESS      0x1a04
#define mmGRPH_PITCH                        0x1a06
#define mmGRPH_PRIMARY_SURFACE_ADDRESS_HIGH 0x1a07
#define mmGRPH_X_END                        0x1a0d
#define mmGRPH_Y_END                        0x1a0e

#define mmCUR_CONTROL                       0x1a66
#define mmCUR_SURFACE_ADDRESS               0x1a67
#define mmCUR_SIZE                          0x1a68
#define mmCUR_SURFACE_ADDRESS_HIGH          0x1a69
#define mmCUR_POSITION                      0x1a6a
#define mmCUR_HOT_SPOT                      0x1a6b
#define mmCUR_UPDATE                        0x1a6e

#define mmSCL_MODE                          0x1b42
#define mmVIEWPORT_SIZE                     0x1b5d /* [29:16] width, [13:0] height */
#define mmCRTC_H_TOTAL                      0x1b80
#define mmCRTC_H_BLANK_START_END            0x1b81 /* [13:0] start, [29:16] end */
#define mmCRTC_V_TOTAL                      0x1b87
#define mmCRTC_V_BLANK_START_END            0x1b8d
#define mmCRTC_CONTROL                      0x1b9c
#define mmCRTC_BLANK_CONTROL                0x1b9d
#define mmCRTC_STATUS_FRAME_COUNT           0x1ba6 /* [23:0] frames scanned out */

#define CRTC_CONTROL__CRTC_MASTER_EN_MASK   0x00000001
#define GRPH_ENABLE__GRPH_ENABLE_MASK       0x00000001
#define CUR_CONTROL__CURSOR_EN_MASK         0x00000001
#define CUR_CONTROL__CURSOR_MODE_MASK       0x00000300
#define CUR_CONTROL__CURSOR_MODE__SHIFT     8
#define CUR_CONTROL__CURSOR_2X_MAGNIFY_MASK 0x00010000
#define CUR_UPDATE__CURSOR_UPDATE_LOCK_MASK 0x00010000

#define CURSOR_MODE_24_8_PREMULT            2

/*
 * The cursor plane fetches its image with a fixed row pitch of 128 pixels
 * (the maximum cursor width), whatever CUR_SIZE says.  Always give it a
 * full 128x128 image.
 */
#define GCN_CURSOR_HW_DIM 128

#define GCN_MAX_CRTC 6

/* per-CRTC register block offsets relative to the CRTC 0 instance */
#define GCN_CRTC_OFFSETS { 0x0000, 0x0200, 0x0400, 0x2600, 0x2800, 0x2a00 }

#endif
