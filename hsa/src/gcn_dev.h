/*
 * gcn_dev.h - user-mode access to the adapter through the gcn4xp display
 * driver: register I/O and video memory I/O.
 */
#ifndef GCN_DEV_H
#define GCN_DEV_H

#include <windows.h>
#include <winioctl.h>
#include "gcn_shared.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

/* Opens the device; returns 0 on success.  Safe to call more than once. */
int gcn_dev_open(void);
void gcn_dev_close(void);
const GCN_INFO *gcn_dev_info(void);
const char *gcn_dev_error(void);

/*
 * Serialises GPU use between processes.  The engine state lives in the
 * hardware, so only one process may drive it at a time.
 */
int gcn_dev_lock(DWORD timeout_ms);
void gcn_dev_unlock(void);

u32 gcn_rreg(u32 index);
void gcn_wreg(u32 index, u32 value);

/*
 * Maps `len` bytes of video memory at `offset` (page aligned, inside the
 * compute half of the aperture) into this process.  Returns NULL when the
 * driver cannot (release builds, older drivers).  While a mapping exists
 * the gcn_vram_* calls below use it for the range it covers.
 */
void *gcn_dev_map(u64 offset, u32 len);
void gcn_dev_unmap(void *va);
/* CPU address of a VRAM offset inside the mapping, or NULL. */
void *gcn_dev_ptr(u64 offset);
/* Makes CPU writes through the mapping visible to the GPU. */
void gcn_dev_flush_writes(void);
/* Makes GPU writes visible to CPU reads through the mapping. */
void gcn_dev_invalidate_reads(void);

/* Video memory, addressed by byte offset from the start of VRAM. */
u32 gcn_vram_rd32(u64 offset);
void gcn_vram_wr32(u64 offset, u32 value);
void gcn_vram_read(u64 offset, void *buf, u32 bytes);
void gcn_vram_write(u64 offset, const void *buf, u32 bytes);
void gcn_vram_fill(u64 offset, u32 value, u32 bytes);

#endif
