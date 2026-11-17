/*
 * gcn_gpu.h - bring-up and command submission for the GCN graphics/compute
 * engine.
 *
 * Video memory layout.  The display driver owns the low part of VRAM
 * (framebuffer, cursor image); everything here lives in the upper half of
 * the PCI aperture so that it can also be reached by the CPU:
 *
 *   region + 0x000000  command ring                 (64 KB)
 *   region + 0x010000  fence / write-back page
 *   region + 0x011000  dummy page for faulting accesses
 *   region + 0x012000  end-of-pipe buffer and queue descriptor
 *   region + 0x020000  scratch for self tests
 *   region + 0x040000  MEC microcode, executed from here
 *   region + 0x100000  page table of the user address space (1 MB)
 *   region + 0x200000  heap: pages backing the user address space (VMID 1)
 */
#ifndef GCN_GPU_H
#define GCN_GPU_H

#include <stdio.h>
#include "gcn_dev.h"

#define GCN_RING_OFF        0x000000
#define GCN_RING_BYTES      0x010000
#define GCN_WB_OFF          0x010000
#define GCN_DUMMY_OFF       0x011000
#define GCN_TEST_OFF        0x020000
#define GCN_PT_OFF          0x100000
#define GCN_PT_BYTES        0x100000
#define GCN_HEAP_OFF        0x200000

#define GCN_GPU_PAGE        4096u
#define GCN_USER_VMID       1

/* VRAM offset of the compute region, and the bytes in it after the heap
 * offset that can back user memory. */
u64 gcn_region_off(void);
u64 gcn_heap_bytes(void);
/* GPU (memory controller) address of a VRAM offset. */
u64 gcn_mc_addr(u64 vram_offset);

/*
 * Brings the engine up if it is not running yet (or unconditionally when
 * `force` is set): loads the microcode from `fw_dir`, starts the RLC and
 * the command processor and runs a ring test.  Progress goes to `log` when
 * it is not NULL.  Returns 0 on success.  Needs the device lock.
 */
int gcn_gpu_init(const char *fw_dir, int force, FILE *log);
int gcn_gpu_is_up(void);
void gcn_gpu_status(FILE *out);

/* Appends packets to the command ring and starts the CP on them. */
void gcn_ring_emit(const u32 *pkt, u32 count);
void gcn_ring_submit(const u32 *pkt, u32 count);
/*
 * Runs an indirect buffer under the given VMID and waits until everything
 * it started, compute waves included, has finished.  Returns 0, or -1 on
 * timeout (the engine is then hung and needs gcn_gpu_init(force)).
 */
int gcn_run_ib(u64 ib_gpu_addr, u32 dwords, u32 vmid, u32 timeout_ms);

/*
 * Queues an indirect buffer on the compute queue without waiting and
 * returns its sequence number.  With `acquire` the GPU caches are written
 * back and invalidated first, so that the shaders see what the CPU wrote.
 * Completion makes all the work's memory writes visible to the CPU.
 */
u32 gcn_gpu_submit(u64 ib_addr, u32 dwords, u32 vmid, int acquire);
/* Sequence number of the last finished submission. */
u32 gcn_gpu_completed(void);
/* Waits for `seq`; 0, or -1 on timeout (see gcn_gpu_error). */
int gcn_gpu_wait(u32 seq, u32 timeout_ms);

/*
 * Points the user VMID at a flat page table mapping `pages` pages starting
 * at virtual address `va` to the heap pages starting at `heap_page`.
 */
void gcn_vm_map(u64 va, u32 pages, u32 heap_page);
/* Prints and clears a pending VM fault; returns nonzero if there was one. */
int gcn_vm_fault(FILE *out);

const char *gcn_gpu_error(void);

#endif
