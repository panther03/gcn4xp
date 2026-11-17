/*
 * hsa_impl.h - internals shared by the parts of the gcn4xp HSA runtime.
 *
 * Memory model.  The compute half of the PCI aperture is mapped into the
 * process once, at hsa_init.  The GPU's user address space (VMID 1) maps
 * the heap part of it at the very same virtual addresses, so a pointer
 * means the same thing to the CPU and to kernels.  Every allocation, from
 * any region, comes from that heap.
 *
 * Queues.  AQL queues are plain host memory.  Ringing the doorbell (or
 * waiting on any signal) runs the packet processor, which turns each
 * kernel dispatch packet into a PM4 indirect buffer for the one hardware
 * compute queue and tracks its completion through the queue's fence.
 */
#ifndef HSA_IMPL_H
#define HSA_IMPL_H

#include "hsa.h"
#include "hsa_ext_amd.h"
#include "gcn_gpu.h"

#define HSA_ISA_NAME   "amdgcn-amd-amdhsa--gfx803"
#define HSA_AGENT_NAME "gfx803"
#define EF_AMDGPU_MACH_GFX803 0x2a

/* debug output, enabled with GCN4XP_TRACE=1 */
extern int g_hsa_trace;
void hsa_trace(const char *fmt, ...);
/* complaints that make a call fail; always printed to stderr */
void hsa_complain(const char *fmt, ...);

int hsa_rt_ready(void);

/* heap */
void *hsa_heap_alloc(size_t size, size_t align);
int hsa_heap_free(void *p);
int hsa_heap_owns(const void *p);

/* kernel symbols, as the packet processor needs them */
typedef struct {
    u32 group_segment_fixed_size;
    u32 private_segment_fixed_size;
    u32 kernarg_size;
    u32 reserved0;
    long long kernel_code_entry_byte_offset;
    u8 reserved1[20];
    u32 compute_pgm_rsrc3;
    u32 compute_pgm_rsrc1;
    u32 compute_pgm_rsrc2;
    u16 kernel_code_properties;
    u16 kernarg_preload;
    u8 reserved2[4];
} hsa_kernel_descriptor;

/* kernel_code_properties */
#define KD_PRIVATE_SEGMENT_BUFFER  0x0001
#define KD_DISPATCH_PTR            0x0002
#define KD_QUEUE_PTR               0x0004
#define KD_KERNARG_SEGMENT_PTR     0x0008
#define KD_DISPATCH_ID             0x0010
#define KD_FLAT_SCRATCH_INIT       0x0020
#define KD_PRIVATE_SEGMENT_SIZE    0x0040
#define KD_WAVEFRONT_SIZE32        0x0400
#define KD_USES_DYNAMIC_STACK      0x0800

#endif
