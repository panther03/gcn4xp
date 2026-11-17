/*
 * gcn_gpu.c - bring-up and command submission for the GCN graphics/compute
 * engine of Polaris parts.
 *
 * The firmware leaves the engine cold: the command processor (CP) and the
 * RLC have no microcode and are halted.  This file loads the microcode,
 * starts the RLC and the first compute micro engine (MEC), and sets up one
 * hardware compute queue that all work is submitted through.  The graphics
 * ring (cp_start) is kept as an experiment, GCN4XP_INIT=0x10; its front end
 * never decodes packets on this setup.
 *
 * The sequence follows the Linux amdgpu driver (gfx_v8_0.c, gmc_v8_0.c).
 */
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "gcn_gpu.h"
#include "gcn_regs.h"

#define RING_DWORDS (GCN_RING_BYTES / 4)

static char g_err[200];
/* bring-up experiments, from GCN4XP_INIT: 1 no reset, 2 no core init,
 * 4 no RLC, 8 plain (not "_2") firmware */
static unsigned g_dbg;
static FILE *g_log;
static u32 g_wptr;
static int g_wptr_valid;
static u32 g_last_seq;      /* last sequence number put on the queue */

const char *gcn_gpu_error(void)
{
    return g_err;
}

static void logf_(const char *fmt, ...)
{
    va_list ap;

    if (g_log == NULL)
        return;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

static int fail(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(g_err, sizeof(g_err), fmt, ap);
    va_end(ap);
    logf_("error: %s\n", g_err);
    return -1;
}

/* ---- layout ------------------------------------------------------------ */

u64 gcn_region_off(void)
{
    return gcn_dev_info()->FbApertureLen / 2;
}

u64 gcn_heap_bytes(void)
{
    return gcn_dev_info()->FbApertureLen - gcn_region_off() - GCN_HEAP_OFF;
}

u64 gcn_mc_addr(u64 vram_offset)
{
    const GCN_INFO *i = gcn_dev_info();

    return (((u64)i->McFbBaseHigh << 32) | i->McFbBaseLow) + vram_offset;
}

static u64 region_mc(u32 off)
{
    return gcn_mc_addr(gcn_region_off() + off);
}

/* ---- firmware files ---------------------------------------------------- */

static int is_dir(const char *p)
{
    DWORD a = GetFileAttributesA(p);

    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

const char *gcn_firmware_dir(void)
{
    static char dir[MAX_PATH + 16];
    char exe[MAX_PATH], *slash;
    const char *env = getenv("GCN4XP_FIRMWARE");

    if (env != NULL && env[0] != 0)
        return env;
    /* next to the program, then next to its parent directory */
    GetModuleFileNameA(NULL, exe, sizeof(exe) - 16);
    slash = strrchr(exe, '\\');
    if (slash != NULL)
        *slash = 0;
    snprintf(dir, sizeof(dir), "%s\\firmware", exe);
    if (is_dir(dir))
        return dir;
    slash = strrchr(exe, '\\');
    if (slash != NULL) {
        *slash = 0;
        snprintf(dir, sizeof(dir), "%s\\firmware", exe);
        if (is_dir(dir))
            return dir;
    }
    /* the SDK's copy */
    env = getenv("GCN4XP_SDK_HOME");
    if (env != NULL && env[0] != 0) {
        snprintf(dir, sizeof(dir), "%s\\firmware", env);
        if (is_dir(dir))
            return dir;
    }
    GetModuleFileNameA(NULL, exe, sizeof(exe) - 16);
    slash = strrchr(exe, '\\');
    if (slash != NULL)
        *slash = 0;
    snprintf(dir, sizeof(dir), "%s\\firmware", exe);
    return dir;     /* not there; the error names it */
}

typedef struct {
    u32 size_bytes;
    u32 header_size_bytes;
    u16 header_version_major;
    u16 header_version_minor;
    u16 ip_version_major;
    u16 ip_version_minor;
    u32 ucode_version;
    u32 ucode_size_bytes;
    u32 ucode_array_offset_bytes;
    u32 crc32;
} fw_header;

typedef struct {
    u8 *data;
    const u32 *code;
    u32 dwords;
    u32 version;
} fw_image;

static int fw_load(const char *dir, const char *chip, const char *part,
                   fw_image *fw)
{
    static const char *const suffix[] = { "_2", "" };
    char path[MAX_PATH];
    FILE *f = NULL;
    long size;
    const fw_header *h;
    int i;

    memset(fw, 0, sizeof(*fw));
    /* The "_2" images are the ones for the later board revisions. */
    for (i = (g_dbg & 8) ? 1 : 0; i < 2 && f == NULL; i++) {
        snprintf(path, sizeof(path), "%s/%s_%s%s.bin", dir, chip, part,
                 suffix[i]);
        f = fopen(path, "rb");
    }
    if (f == NULL)
        return fail("firmware %s/%s_%s.bin not found", dir, chip, part);
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    fw->data = (u8 *)malloc((size_t)size);
    if (fw->data == NULL || fread(fw->data, 1, (size_t)size, f) !=
        (size_t)size) {
        fclose(f);
        return fail("cannot read %s", path);
    }
    fclose(f);
    h = (const fw_header *)fw->data;
    if (size < (long)sizeof(*h) || h->size_bytes != (u32)size ||
        h->ucode_array_offset_bytes > (u32)size ||
        h->ucode_size_bytes > (u32)size - h->ucode_array_offset_bytes)
        return fail("%s is not a valid firmware image", path);
    fw->code = (const u32 *)(fw->data + h->ucode_array_offset_bytes);
    fw->dwords = h->ucode_size_bytes / 4;
    fw->version = h->ucode_version;
    logf_("  %s: version %u, %u words\n", path, fw->version, fw->dwords);
    return 0;
}

static void fw_free(fw_image *fw)
{
    free(fw->data);
    fw->data = NULL;
}

static void fw_write(u32 addr_reg, u32 data_reg, const fw_image *fw)
{
    u32 i;

    gcn_wreg(addr_reg, 0);
    for (i = 0; i < fw->dwords; i++)
        gcn_wreg(data_reg, fw->code[i]);
    gcn_wreg(addr_reg, fw->version);
}

/* ---- helpers ----------------------------------------------------------- */

static void udelay_(u32 us)
{
    Sleep((us + 999) / 1000);
}

static void srbm_select_vmid(u32 vmid)
{
    gcn_wreg(mmSRBM_GFX_CNTL, vmid << 4);
}

static void reg_rmw(u32 reg, u32 and_mask, u32 or_mask)
{
    u32 v;

    if (and_mask == 0xffffffff) {
        v = or_mask;
    } else {
        v = gcn_rreg(reg);
        v = (v & ~and_mask) | (or_mask & and_mask);
    }
    gcn_wreg(reg, v);
}

/* ---- memory controller ------------------------------------------------- */

#define PTE_VALID    0x01
#define PTE_EXEC     0x10
#define PTE_READ     0x20
#define PTE_WRITE    0x40

static void mc_init(void)
{
    const GCN_INFO *i = gcn_dev_info();
    u64 base = gcn_mc_addr(0);
    u64 top = base + (u64)i->VramMB * 1024 * 1024 - 1;
    u32 v;

    /*
     * Addresses inside the system aperture are used untranslated by VMID 0
     * (the ring and its fences).  The firmware leaves the aperture empty.
     */
    gcn_wreg(mmMC_VM_SYSTEM_APERTURE_LOW_ADDR, (u32)(base >> 12));
    gcn_wreg(mmMC_VM_SYSTEM_APERTURE_HIGH_ADDR, (u32)(top >> 12));
    gcn_wreg(mmMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR,
             (u32)(region_mc(GCN_DUMMY_OFF) >> 12));

    v = gcn_rreg(mmMC_VM_MX_L1_TLB_CNTL);
    v |= 0x01 | 0x02;   /* L1 TLB, fragment processing */
    v |= 0x18;          /* system access mode: outside aperture is mapped */
    v &= ~0x20u;        /* no unmapped access */
    v |= 0x40;          /* advanced driver model */
    gcn_wreg(mmMC_VM_MX_L1_TLB_CNTL, v);

    v = gcn_rreg(mmVM_L2_CNTL);
    v |= 0x001 | 0x002 | 0x200 | 0x400 | 0x800;
    v = (v & ~0x38000u) | (7u << 15);       /* L2 queue size */
    v = (v & ~0x180000u) | (1u << 19);      /* context 1 identity mode */
    gcn_wreg(mmVM_L2_CNTL, v);
    gcn_wreg(mmVM_L2_CNTL2, gcn_rreg(mmVM_L2_CNTL2) | 0x3);
    v = gcn_rreg(mmVM_L2_CNTL3);
    v = (v & ~0x3fu) | 4;                   /* bank select */
    v = (v & ~0xf8000u) | (4u << 15);       /* big-K fragment size */
    v |= 0x100000;                          /* big-K associativity */
    gcn_wreg(mmVM_L2_CNTL3, v);
    gcn_wreg(mmVM_L2_CNTL4, 0);

    gcn_wreg(mmVM_CONTEXT0_PROTECTION_FAULT_DEFAULT_ADDR,
             (u32)(region_mc(GCN_DUMMY_OFF) >> 12));
    gcn_wreg(mmVM_CONTEXT1_PROTECTION_FAULT_DEFAULT_ADDR,
             (u32)(region_mc(GCN_DUMMY_OFF) >> 12));
}

void gcn_vm_map(u64 va, u32 pages, u32 heap_page)
{
    u64 pt = gcn_region_off() + GCN_PT_OFF;
    /*
     * Page table entries of a discrete part hold the VRAM offset, not the
     * MC address (amdgpu: vram_base_offset is 0 unless it is an APU); the
     * table base register on the other hand takes the MC address.
     */
    u64 phys = gcn_region_off() + GCN_HEAP_OFF + (u64)heap_page * GCN_GPU_PAGE;
    u32 i, v;

    if (pages > GCN_PT_BYTES / 8)
        pages = GCN_PT_BYTES / 8;
    for (i = 0; i < pages; i++) {
        u64 pte = (phys + (u64)i * GCN_GPU_PAGE) | PTE_VALID | PTE_READ |
                  PTE_WRITE | PTE_EXEC;
        gcn_vram_wr32(pt + (u64)i * 8, (u32)pte);
        gcn_vram_wr32(pt + (u64)i * 8 + 4, (u32)(pte >> 32));
    }

    /* Contexts 1..15 share the range registers; only context 1 is used. */
    gcn_wreg(mmVM_CONTEXT1_PAGE_TABLE_START_ADDR, (u32)(va >> 12));
    gcn_wreg(mmVM_CONTEXT1_PAGE_TABLE_END_ADDR, (u32)(va >> 12) + pages - 1);
    gcn_wreg(mmVM_CONTEXT0_PAGE_TABLE_BASE_ADDR + GCN_USER_VMID,
             (u32)(region_mc(GCN_PT_OFF) >> 12));
    gcn_wreg(mmVM_CONTEXT1_CNTL2, 4);
    /*
     * Flat table, faulting accesses go to the dummy page.  No fault
     * interrupts: nothing services the interrupt line.
     */
    v = 0x00000001                        /* enable, depth 0 */
        | 0x10 | 0x80 | 0x400 | 0x2000 | 0x10000 | 0x80000 | 0x400000;
    gcn_wreg(mmVM_CONTEXT1_CNTL, v);

    gcn_wreg(mmHDP_MEM_COHERENCY_FLUSH_CNTL, 0);
    gcn_wreg(mmVM_INVALIDATE_REQUEST, 1u << GCN_USER_VMID);
    (void)gcn_rreg(mmVM_INVALIDATE_RESPONSE);
}

int gcn_vm_fault(FILE *out)
{
    u32 st = gcn_rreg(mmVM_CONTEXT1_PROTECTION_FAULT_STATUS);
    u32 addr, client;

    if (st == 0)
        return 0;
    addr = gcn_rreg(mmVM_CONTEXT1_PROTECTION_FAULT_ADDR);
    client = gcn_rreg(mmVM_CONTEXT1_PROTECTION_FAULT_MCCLIENT);
    if (out != NULL)
        fprintf(out,
                "GPU memory fault: %s at address 0x%llx (vmid %u, client "
                "%08x, protection 0x%02x)\n",
                (st & 0x01000000) ? "write" : "read",
                (unsigned long long)addr << 12, (st >> 25) & 0xf, client,
                st & 0xff);
    /* clear: write 1 to CONTEXT1_CNTL2 bit 0 */
    gcn_wreg(mmVM_CONTEXT1_CNTL2, gcn_rreg(mmVM_CONTEXT1_CNTL2) | 1);
    return 1;
}

/* ---- shader core defaults ---------------------------------------------- */

static const u32 g_golden[] = {
    /* register, mask, value: Polaris10 settings from amdgpu */
    0x0cd4 /* ATC_MISC_CG */,        0x000c0fc0, 0x000c0200,
    0x2300 /* SQ_CONFIG */,          0x07f80000, 0x07180000,
    0x2542 /* TA_CNTL_AUX */,        0x000f000f, 0x000b0000,
    0x2b80 /* TCC_CTRL */,           0x00100000, 0xf31fff7f,
    0x2b05 /* TCP_ADDR_CONFIG */,    0x000003ff, 0x000000f7,
    0x2b04 /* TCP_CHAN_STEER_HI */,  0xffffffff, 0x00000000,
    0xc200 /* GRBM_GFX_INDEX */,     0xffffffff, 0xe0000000,
    0x31dc /* SPI_RESOURCE_RESERVE_CU_0 */,    0xffffffff, 0x00000800,
    0x31dd /* SPI_RESOURCE_RESERVE_CU_1 */,    0xffffffff, 0x00000800,
    0x31e6 /* SPI_RESOURCE_RESERVE_EN_CU_0 */, 0xffffffff, 0x00ff7fbf,
    0x31e7 /* SPI_RESOURCE_RESERVE_EN_CU_1 */, 0xffffffff, 0x00ff7faf,
};

static void core_init(void)
{
    u32 i, vmid;

    for (i = 0; i < sizeof(g_golden) / sizeof(g_golden[0]); i += 3)
        reg_rmw(g_golden[i], g_golden[i + 1], g_golden[i + 2]);

    gcn_wreg(mmGRBM_CNTL, (gcn_rreg(mmGRBM_CNTL) & ~0xffu) | 0xff);

    /*
     * Shader memory model, per VMID: 64-bit flat addressing, with the LDS
     * and scratch apertures parked high up (0x6000'0000'0000'0000) so the
     * whole low address range is ordinary global memory.
     */
    for (vmid = 0; vmid < 16; vmid++) {
        srbm_select_vmid(vmid);
        gcn_wreg(mmSH_MEM_CONFIG,
                 SH_MEM_CONFIG_ALIGNMENT_UNALIGNED |
                 SH_MEM_CONFIG_DEFAULT_MTYPE(vmid == 0 ? MTYPE_UC : MTYPE_NC) |
                 SH_MEM_CONFIG_APE1_MTYPE(vmid == 0 ? MTYPE_UC : MTYPE_NC));
        gcn_wreg(mmSH_MEM_APE1_BASE, 1);
        gcn_wreg(mmSH_MEM_APE1_LIMIT, 0);
        gcn_wreg(mmSH_MEM_BASES, 0x60006000);
    }
    srbm_select_vmid(0);
}

/* ---- RLC --------------------------------------------------------------- */

static void rlc_wait_serdes(void)
{
    int i;

    for (i = 0; i < 100; i++) {
        if (gcn_rreg(mmRLC_SERDES_CU_MASTER_BUSY) == 0 &&
            (gcn_rreg(mmRLC_SERDES_NONCU_MASTER_BUSY) & 0xf) == 0)
            return;
        udelay_(1000);
    }
    logf_("  warning: RLC serdes still busy\n");
}

static int rlc_start(const char *fw_dir)
{
    fw_image rlc;

    if (fw_load(fw_dir, "polaris10", "rlc", &rlc) != 0) {
        /* there is no "_2" RLC image; fw_load tried both names */
        return -1;
    }

    gcn_wreg(mmRLC_CNTL, gcn_rreg(mmRLC_CNTL) & ~1u);
    rlc_wait_serdes();

    /* no clock or power gating: nothing here manages them */
    gcn_wreg(mmRLC_CGCG_CGLS_CTRL, gcn_rreg(mmRLC_CGCG_CGLS_CTRL) & ~3u);
    gcn_wreg(mmRLC_CGCG_CGLS_CTRL_3D,
             gcn_rreg(mmRLC_CGCG_CGLS_CTRL_3D) & ~3u);
    gcn_wreg(mmRLC_PG_CNTL, 0);

    gcn_wreg(mmGRBM_SOFT_RESET,
             gcn_rreg(mmGRBM_SOFT_RESET) | GRBM_SOFT_RESET_RLC);
    udelay_(1000);
    gcn_wreg(mmGRBM_SOFT_RESET,
             gcn_rreg(mmGRBM_SOFT_RESET) & ~GRBM_SOFT_RESET_RLC);
    udelay_(1000);

    fw_write(mmRLC_GPM_UCODE_ADDR, mmRLC_GPM_UCODE_DATA, &rlc);
    fw_free(&rlc);

    gcn_wreg(mmRLC_CNTL, gcn_rreg(mmRLC_CNTL) | 1);
    udelay_(1000);
    logf_("  RLC started: cntl %08x stat %08x gpm_stat %08x\n",
          gcn_rreg(mmRLC_CNTL), gcn_rreg(mmRLC_STAT),
          gcn_rreg(mmRLC_GPM_STAT));
    return 0;
}

/* ---- command processor ------------------------------------------------- */

static void cp_halt(int halt)
{
    u32 v = gcn_rreg(mmCP_ME_CNTL);
    u32 bits = CP_ME_CNTL_ME_HALT | CP_ME_CNTL_PFP_HALT | CP_ME_CNTL_CE_HALT;

    gcn_wreg(mmCP_ME_CNTL, halt ? (v | bits) : (v & ~bits));
    udelay_(1000);
}

void gcn_ring_emit(const u32 *pkt, u32 count)
{
    u64 ring = gcn_region_off() + GCN_RING_OFF;
    u32 i;

    if (!g_wptr_valid) {
        g_wptr = gcn_rreg(mmCP_RB0_WPTR) % RING_DWORDS;
        g_wptr_valid = 1;
    }
    for (i = 0; i < count; i++) {
        gcn_vram_wr32(ring + (u64)g_wptr * 4, pkt[i]);
        g_wptr = (g_wptr + 1) % RING_DWORDS;
    }
    /* the CP fetches in blocks; keep the write pointer aligned */
    while (g_wptr & 7) {
        gcn_vram_wr32(ring + (u64)g_wptr * 4, PACKET3_NOP_DWORD);
        g_wptr = (g_wptr + 1) % RING_DWORDS;
    }
    gcn_wreg(mmHDP_MEM_COHERENCY_FLUSH_CNTL, 0);
    gcn_wreg(mmCP_RB0_WPTR, g_wptr);
    (void)gcn_rreg(mmCP_RB0_WPTR);
}

/* Waits until SCRATCH_REG1 shows `seq`. */
static int ring_wait(u32 seq, u32 timeout_ms)
{
    DWORD start = GetTickCount();
    u32 spins = 0;

    for (;;) {
        if (gcn_rreg(mmSCRATCH_REG1) == seq)
            return 0;
        if (GetTickCount() - start > timeout_ms)
            return -1;
        if (++spins > 200)
            Sleep(1);
    }
}

static u32 next_seq(void)
{
    u32 seq = gcn_rreg(mmSCRATCH_REG1) + 1;

    return seq == 0 ? 1 : seq;
}

static int ring_test(void)
{
    u32 pkt[3];
    u32 seq = next_seq();

    pkt[0] = PACKET3(PACKET3_SET_UCONFIG_REG, 1);
    pkt[1] = mmSCRATCH_REG1 - SET_UCONFIG_REG_START;
    pkt[2] = seq;
    gcn_ring_emit(pkt, 3);
    if (ring_wait(seq, 1000) != 0)
        return fail("ring test failed: scratch %08x, expected %08x "
                    "(rptr %x wptr %x, CP_STAT %08x, GRBM_STATUS %08x)",
                    gcn_rreg(mmSCRATCH_REG1), seq, gcn_rreg(mmCP_RB0_RPTR),
                    gcn_rreg(mmCP_RB0_WPTR), gcn_rreg(mmCP_STAT),
                    gcn_rreg(mmGRBM_STATUS));
    return 0;
}

static int cp_start(const char *fw_dir)
{
    fw_image pfp, me, ce;
    u64 ring = region_mc(GCN_RING_OFF);
    u64 wb = region_mc(GCN_WB_OFF);
    u32 bufsz, cntl, n;
    u32 pkt[16];
    int r;

    cp_halt(1);

    r = fw_load(fw_dir, "polaris10", "pfp", &pfp);
    if (r == 0)
        r = fw_load(fw_dir, "polaris10", "me", &me);
    if (r == 0)
        r = fw_load(fw_dir, "polaris10", "ce", &ce);
    if (r != 0)
        return r;
    fw_write(mmCP_PFP_UCODE_ADDR, mmCP_PFP_UCODE_DATA, &pfp);
    fw_write(mmCP_CE_UCODE_ADDR, mmCP_CE_UCODE_DATA, &ce);
    fw_write(mmCP_ME_RAM_WADDR, mmCP_ME_RAM_DATA, &me);
    fw_free(&pfp);
    fw_free(&me);
    fw_free(&ce);

    /* the ring: all of it NOPs so stale contents can never execute */
    gcn_vram_fill(gcn_region_off() + GCN_RING_OFF, PACKET3_NOP_DWORD,
                  GCN_RING_BYTES);
    gcn_vram_fill(gcn_region_off() + GCN_WB_OFF, 0, 2 * GCN_GPU_PAGE);

    gcn_wreg(mmCP_INT_CNTL_RING0, 0);   /* nothing services interrupts */
    gcn_wreg(mmCP_RB_WPTR_DELAY, 0);
    gcn_wreg(mmCP_RB_VMID, 0);

    for (bufsz = 0; (8u << bufsz) < GCN_RING_BYTES; bufsz++)
        ;
    cntl = bufsz | ((bufsz - 2) << 8) | (3u << 15) | (1u << 22);
    gcn_wreg(mmCP_RB0_CNTL, cntl | CP_RB0_CNTL_RB_RPTR_WR_ENA);
    gcn_wreg(mmCP_RB0_WPTR, 0);
    g_wptr = 0;
    g_wptr_valid = 1;
    gcn_wreg(mmCP_RB0_RPTR_ADDR, (u32)wb & 0xfffffffc);
    gcn_wreg(mmCP_RB0_RPTR_ADDR_HI, (u32)(wb >> 32) & 0xff);
    udelay_(1000);
    gcn_wreg(mmCP_RB0_CNTL, cntl);
    gcn_wreg(mmCP_RB0_BASE, (u32)(ring >> 8));
    gcn_wreg(mmCP_RB0_BASE_HI, (u32)(ring >> 40));
    gcn_wreg(mmCP_RB_DOORBELL_CONTROL,
             gcn_rreg(mmCP_RB_DOORBELL_CONTROL) & ~(1u << 30));

    gcn_wreg(mmCP_MAX_CONTEXT, 8 - 1);
    gcn_wreg(mmCP_ENDIAN_SWAP, 0);
    gcn_wreg(mmCP_DEVICE_ID, 1);

    cp_halt(0);

    /*
     * No graphics preamble: CLEAR_STATE needs a clear-state buffer set up
     * through the RLC, and nothing here draws.  Compute state is set per
     * dispatch.
     */
    n = 0;
    pkt[n++] = PACKET3(PACKET3_CONTEXT_CONTROL, 1);
    pkt[n++] = 0x80000000;
    pkt[n++] = 0x80000000;
    gcn_ring_emit(pkt, n);

    return ring_test();
}

/* ---- compute micro engine (MEC) queue ---------------------------------- */

/*
 * All work goes through one hardware queue descriptor: ME1, pipe 0, queue 0,
 * programmed directly through registers.  Its write pointer is both written
 * to the HQD register and mirrored at a memory location the MEC polls, so
 * no doorbell (a PCI BAR that user mode cannot reach) is needed.
 */
#define MEC_ME       1
#define MEC_PIPE     0
#define MEC_QUEUE    0
#define WB_RPTR      0x000   /* offsets in the write-back page */
#define WB_WPTR_POLL 0x040
#define WB_FENCE     0x080
#define GCN_EOP_OFF  0x012000
#define GCN_EOP_BYTES 2048
#define GCN_MQD_OFF  0x013000
#define GCN_MECFW_OFF   0x040000   /* MEC microcode, executed from here */
#define GCN_MECFW_BYTES 0x0c0000

static int g_use_gfx;       /* experiment: graphics ring instead of MEC */

static void srbm_select(u32 me, u32 pipe, u32 queue, u32 vmid)
{
    gcn_wreg(mmSRBM_GFX_CNTL,
             (pipe & 3) | ((me & 3) << 2) | ((vmid & 0xf) << 4) |
             ((queue & 7) << 8));
}

static void mec_halt(int halt)
{
    gcn_wreg(mmCP_MEC_CNTL, halt ? (CP_MEC_ME1_HALT | CP_MEC_ME2_HALT)
                                 : CP_MEC_ME2_HALT);
    udelay_(1000);
}

static void mec_emit(const u32 *pkt, u32 count)
{
    u64 ring = gcn_region_off() + GCN_RING_OFF;
    u32 i;

    if (!g_wptr_valid) {
        g_wptr = gcn_vram_rd32(gcn_region_off() + GCN_WB_OFF + WB_WPTR_POLL)
                 % RING_DWORDS;
        g_wptr_valid = 1;
    }
    for (i = 0; i < count; i++) {
        gcn_vram_wr32(ring + (u64)g_wptr * 4, pkt[i]);
        g_wptr = (g_wptr + 1) % RING_DWORDS;
    }
    gcn_dev_flush_writes();
    gcn_vram_wr32(gcn_region_off() + GCN_WB_OFF + WB_WPTR_POLL, g_wptr);
    gcn_dev_flush_writes();
    srbm_select(MEC_ME, MEC_PIPE, MEC_QUEUE, 0);
    gcn_wreg(mmCP_HQD_PQ_WPTR, g_wptr);
    srbm_select(0, 0, 0, 0);
}

static u32 mec_rreg(u32 reg)
{
    u32 v;

    srbm_select(MEC_ME, MEC_PIPE, MEC_QUEUE, 0);
    v = gcn_rreg(reg);
    srbm_select(0, 0, 0, 0);
    return v;
}

static u32 fence_value(void)
{
    u64 off = gcn_region_off() + GCN_WB_OFF + WB_FENCE;

    if (gcn_dev_ptr(off) != NULL)
        gcn_dev_invalidate_reads();
    return gcn_vram_rd32(off);
}

/* RELEASE_MEM: after all prior work, flush caches and write `seq`. */
static u32 emit_fence(u32 *pkt, u32 seq)
{
    u64 addr = region_mc(GCN_WB_OFF + WB_FENCE);
    u32 n = 0;

    pkt[n++] = PACKET3(PACKET3_RELEASE_MEM, 5);
    pkt[n++] = EOP_TCL1_ACTION_EN | EOP_TC_ACTION_EN | EOP_TC_WB_ACTION_EN |
               EVENT_TYPE(CACHE_FLUSH_AND_INV_TS_EVENT) | EVENT_INDEX(5);
    pkt[n++] = DATA_SEL(1) | INT_SEL(0);
    pkt[n++] = (u32)addr & 0xfffffffc;
    pkt[n++] = (u32)(addr >> 32);
    pkt[n++] = seq;
    pkt[n++] = 0;
    return n;
}

static int fence_wait(u32 seq, u32 timeout_ms)
{
    DWORD start = GetTickCount();
    u32 spins = 0;

    for (;;) {
        if ((int)(fence_value() - seq) >= 0)
            return 0;
        if (GetTickCount() - start > timeout_ms)
            return -1;
        if (++spins > 200)
            Sleep(1);
    }
}

static void mec_status(FILE *out)
{
    srbm_select(MEC_ME, MEC_PIPE, MEC_QUEUE, 0);
    fprintf(out, "HQD           active %x base %08x cntl %08x rptr %x wptr %x "
            "err %08x\n", gcn_rreg(mmCP_HQD_ACTIVE), gcn_rreg(mmCP_HQD_PQ_BASE),
            gcn_rreg(mmCP_HQD_PQ_CONTROL), gcn_rreg(mmCP_HQD_PQ_RPTR),
            gcn_rreg(mmCP_HQD_PQ_WPTR), gcn_rreg(mmCP_HQD_ERROR));
    fprintf(out, "HQD IB        %08x:%08x rptr %x cntl %08x  dequeue %x\n",
            gcn_rreg(mmCP_HQD_IB_BASE_ADDR_HI), gcn_rreg(mmCP_HQD_IB_BASE_ADDR),
            gcn_rreg(mmCP_HQD_IB_RPTR), gcn_rreg(mmCP_HQD_IB_CONTROL),
            gcn_rreg(mmCP_HQD_DEQUEUE_REQUEST));
    fprintf(out, "HQD EOP       cntl %08x rptr %08x wptr %08x mem %08x\n",
            gcn_rreg(mmCP_HQD_EOP_CONTROL), gcn_rreg(mmCP_HQD_EOP_RPTR),
            gcn_rreg(mmCP_HQD_EOP_WPTR), gcn_rreg(mmCP_HQD_EOP_WPTR_MEM));
    srbm_select(0, 0, 0, 0);
    fprintf(out, "CPC           status %08x busy %08x stalled %08x hdr %08x\n",
            gcn_rreg(mmCP_CPC_STATUS), gcn_rreg(mmCP_CPC_BUSY_STAT),
            gcn_rreg(mmCP_CPC_STALLED_STAT1), gcn_rreg(mmCP_MEC_ME1_HEADER_DUMP));
    fprintf(out, "CPF           status %08x busy %08x stalled %08x\n",
            gcn_rreg(mmCP_CPF_STATUS), gcn_rreg(mmCP_CPF_BUSY_STAT),
            gcn_rreg(mmCP_CPF_STALLED_STAT1));
    fprintf(out, "fence         %08x  wptr poll %08x  rptr report %08x\n",
            fence_value(),
            gcn_vram_rd32(gcn_region_off() + GCN_WB_OFF + WB_WPTR_POLL),
            gcn_vram_rd32(gcn_region_off() + GCN_WB_OFF + WB_RPTR));
}

static int mec_test(void)
{
    u32 pkt[16], n = 0;
    u32 seq = fence_value() + 1;

    n = emit_fence(pkt, seq);
    mec_emit(pkt, n);
    if (fence_wait(seq, 1000) != 0) {
        fail("compute queue test failed: fence %08x, expected %08x",
             fence_value(), seq);
        if (g_log != NULL)
            mec_status(g_log);
        return -1;
    }
    return 0;
}

/*
 * The MEC executes its microcode from memory through an instruction cache
 * (CP_CPC_IC_BASE); only the jump table at the end of the image goes into
 * the on-chip RAM.  The image is too large for the RAM port anyway.
 */
static int mec_load(const fw_image *fw)
{
    const u32 *gfx = (const u32 *)(fw->data + sizeof(fw_header));
    u32 jt_off = gfx[1], jt_size = gfx[2];
    u64 img = gcn_region_off() + GCN_MECFW_OFF;
    u64 img_mc = gcn_mc_addr(img);
    u32 i, same = 1;

    if (jt_off + jt_size > fw->dwords || fw->dwords * 4 > GCN_MECFW_BYTES)
        return fail("unexpected MEC firmware layout (%u words, jump table "
                    "%u+%u)", fw->dwords, jt_off, jt_size);
    /* uploading takes seconds; skip it when the image is already there */
    for (i = 0; i < fw->dwords && same; i += 997)
        same = gcn_vram_rd32(img + (u64)i * 4) == fw->code[i];
    same = same && gcn_vram_rd32(img + (u64)(fw->dwords - 1) * 4) ==
                   fw->code[fw->dwords - 1];
    if (!same) {
        logf_("  uploading MEC microcode to video memory\n");
        for (i = 0; i < fw->dwords; i++)
            gcn_vram_wr32(img + (u64)i * 4, fw->code[i]);
    }
    gcn_wreg(mmHDP_MEM_COHERENCY_FLUSH_CNTL, 0);

    gcn_wreg(mmCP_CPC_IC_BASE_CNTL, 0);     /* VMID 0, default policy */
    gcn_wreg(mmCP_CPC_IC_BASE_LO, (u32)img_mc & 0xfffff000);
    gcn_wreg(mmCP_CPC_IC_BASE_HI, (u32)(img_mc >> 32));
    gcn_wreg(mmCP_MEC_CNTL, gcn_rreg(mmCP_MEC_CNTL) | CP_MEC_INVALIDATE_ICACHE);
    udelay_(1000);
    gcn_wreg(mmCP_MEC_CNTL, gcn_rreg(mmCP_MEC_CNTL) &
             ~CP_MEC_INVALIDATE_ICACHE);

    gcn_wreg(mmCP_MEC_ME1_UCODE_ADDR, jt_off);
    for (i = 0; i < jt_size; i++)
        gcn_wreg(mmCP_MEC_ME1_UCODE_DATA, fw->code[jt_off + i]);
    gcn_wreg(mmCP_MEC_ME1_UCODE_ADDR, fw->version);
    return 0;
}

static int mec_start(const char *fw_dir)
{
    fw_image mec;
    u64 ring = region_mc(GCN_RING_OFF);
    u64 wb = region_mc(GCN_WB_OFF);
    u64 eop = region_mc(GCN_EOP_OFF);
    u64 mqd = region_mc(GCN_MQD_OFF);
    u32 qsize, v;
    int i;

    mec_halt(1);
    if (fw_load(fw_dir, "polaris10", "mec", &mec) != 0)
        return -1;
    if (mec_load(&mec) != 0) {
        fw_free(&mec);
        return -1;
    }
    fw_free(&mec);

    gcn_vram_fill(gcn_region_off() + GCN_RING_OFF, PACKET3_NOP_DWORD,
                  GCN_RING_BYTES);
    gcn_vram_fill(gcn_region_off() + GCN_WB_OFF, 0, GCN_GPU_PAGE);
    gcn_vram_fill(gcn_region_off() + GCN_EOP_OFF, 0, GCN_EOP_BYTES);
    gcn_vram_fill(gcn_region_off() + GCN_MQD_OFF, 0, 1024);
    g_wptr = 0;
    g_wptr_valid = 1;

    /*
     * The queue descriptor survives engine resets, half-finished indirect
     * buffers included.  Retire the queue and start from a clean slate, as
     * a fresh MQD commit would.
     */
    srbm_select(MEC_ME, MEC_PIPE, MEC_QUEUE, 0);
    gcn_wreg(mmCP_PQ_WPTR_POLL_CNTL, 0);
    if (gcn_rreg(mmCP_HQD_ACTIVE) & 1) {
        gcn_wreg(mmCP_HQD_DEQUEUE_REQUEST, 2);
        for (i = 0; i < 100 && (gcn_rreg(mmCP_HQD_ACTIVE) & 1); i++)
            udelay_(1000);
    }
    gcn_wreg(mmCP_HQD_ACTIVE, 0);
    gcn_wreg(mmCP_HQD_DEQUEUE_REQUEST, 0);
    /* no indirect buffer in progress; memory type UC as amdgpu sets it */
    gcn_wreg(mmCP_HQD_IB_BASE_ADDR, 0);
    gcn_wreg(mmCP_HQD_IB_BASE_ADDR_HI, 0);
    gcn_wreg(mmCP_HQD_IB_RPTR, 0);
    gcn_wreg(mmCP_HQD_IB_CONTROL, HQD_IB_MIN_AVAIL_SIZE(3) | HQD_IB_MTYPE(3));
    /* power-on defaults of the other descriptor registers */
    gcn_wreg(mmCP_HQD_IQ_TIMER, 0x18000000);
    gcn_wreg(mmCP_MQD_CONTROL, 0x18000100);
    gcn_wreg(mmCP_HQD_PERSISTENT_STATE, 0x00005301);
    /* empty end-of-pipe queue */
    gcn_wreg(mmCP_HQD_EOP_RPTR, 0x40000000);
    gcn_wreg(mmCP_HQD_EOP_WPTR, 0x007f0000);
    gcn_wreg(mmCP_HQD_EOP_WPTR_MEM, 0);
    gcn_wreg(mmCP_HQD_EOP_EVENTS, 0);

    /* the pipe: end-of-pipe event buffer */
    gcn_wreg(mmCP_HQD_EOP_BASE_ADDR, (u32)(eop >> 8));
    gcn_wreg(mmCP_HQD_EOP_BASE_ADDR_HI, (u32)(eop >> 40));
    gcn_wreg(mmCP_HQD_VMID, 0);
    for (qsize = 0; (4u << qsize) < GCN_EOP_BYTES; qsize++)
        ;
    gcn_wreg(mmCP_HQD_EOP_CONTROL, HQD_EOP_MTYPE(3) | (qsize - 1));

    /* the queue */
    gcn_wreg(mmCP_HQD_PQ_DOORBELL_CONTROL, 0);
    gcn_wreg(mmCP_HQD_PQ_RPTR, 0);
    gcn_wreg(mmCP_HQD_PQ_WPTR, 0);
    gcn_wreg(mmCP_MQD_BASE_ADDR, (u32)mqd & 0xfffffffc);
    gcn_wreg(mmCP_MQD_BASE_ADDR_HI, (u32)(mqd >> 32));
    gcn_wreg(mmCP_HQD_PQ_BASE, (u32)(ring >> 8));
    gcn_wreg(mmCP_HQD_PQ_BASE_HI, (u32)(ring >> 40));
    for (qsize = 0; (4u << qsize) < GCN_RING_BYTES; qsize++)
        ;
    /* report the read pointer every 4 KB of progress */
    v = (qsize - 1) | (9u << 8) | HQD_PQ_MTYPE(3) | HQD_PQ_MIN_AVAIL_SIZE(3) |
        HQD_PQ_PRIV_STATE | HQD_PQ_KMD_QUEUE;
    gcn_wreg(mmCP_HQD_PQ_CONTROL, v);
    gcn_wreg(mmCP_HQD_PQ_RPTR_REPORT_ADDR, (u32)(wb + WB_RPTR) & 0xfffffffc);
    gcn_wreg(mmCP_HQD_PQ_RPTR_REPORT_ADDR_HI, (u32)((wb + WB_RPTR) >> 32) &
             0xffff);
    gcn_wreg(mmCP_HQD_PQ_WPTR_POLL_ADDR, (u32)(wb + WB_WPTR_POLL) &
             0xfffffffc);
    gcn_wreg(mmCP_HQD_PQ_WPTR_POLL_ADDR_HI,
             (u32)((wb + WB_WPTR_POLL) >> 32) & 0xffff);
    gcn_wreg(mmCP_HQD_VMID, 0);
    gcn_wreg(mmCP_HQD_ACTIVE, 1);
    srbm_select(0, 0, 0, 0);

    /* the MEC reads the write pointer from memory, so no doorbell */
    gcn_wreg(mmCP_PQ_WPTR_POLL_CNTL, CP_PQ_WPTR_POLL_EN | 1);

    mec_halt(0);
    logf_("  compute queue started: ME1 pipe 0 queue 0\n");
    return mec_test();
}

int gcn_gpu_is_up(void)
{
    if (g_use_gfx) {
        u32 halt = CP_ME_CNTL_ME_HALT | CP_ME_CNTL_PFP_HALT |
                   CP_ME_CNTL_CE_HALT;

        return (gcn_rreg(mmCP_ME_CNTL) & halt) == 0 &&
               gcn_rreg(mmCP_RB0_BASE) ==
               (u32)(region_mc(GCN_RING_OFF) >> 8) &&
               (gcn_rreg(mmRLC_CNTL) & 1) != 0;
    }
    return (gcn_rreg(mmCP_MEC_CNTL) & CP_MEC_ME1_HALT) == 0 &&
           (gcn_rreg(mmRLC_CNTL) & 1) != 0 &&
           (mec_rreg(mmCP_HQD_ACTIVE) & 1) != 0 &&
           mec_rreg(mmCP_HQD_PQ_BASE) == (u32)(region_mc(GCN_RING_OFF) >> 8);
}

static void engine_reset(void)
{
    u32 bits = GRBM_SOFT_RESET_CP | GRBM_SOFT_RESET_GFX | GRBM_SOFT_RESET_RLC |
               GRBM_SOFT_RESET_CPF | GRBM_SOFT_RESET_CPC |
               GRBM_SOFT_RESET_CPG;
    int i;

    gcn_wreg(mmRLC_CNTL, gcn_rreg(mmRLC_CNTL) & ~1u);
    cp_halt(1);
    /* drop the queue, discarding whatever it was doing */
    srbm_select(MEC_ME, MEC_PIPE, MEC_QUEUE, 0);
    if (gcn_rreg(mmCP_HQD_ACTIVE) & 1) {
        gcn_wreg(mmCP_HQD_DEQUEUE_REQUEST, 2);
        for (i = 0; i < 100 && (gcn_rreg(mmCP_HQD_ACTIVE) & 1); i++)
            udelay_(1000);
    }
    srbm_select(0, 0, 0, 0);
    mec_halt(1);
    gcn_wreg(mmCP_PQ_WPTR_POLL_CNTL, 0);

    gcn_wreg(mmGMCON_DEBUG, gcn_rreg(mmGMCON_DEBUG) | GMCON_DEBUG_GFX_STALL |
             GMCON_DEBUG_GFX_CLEAR);
    udelay_(1000);
    gcn_wreg(mmGRBM_SOFT_RESET, gcn_rreg(mmGRBM_SOFT_RESET) | bits);
    (void)gcn_rreg(mmGRBM_SOFT_RESET);
    udelay_(1000);
    gcn_wreg(mmGRBM_SOFT_RESET, gcn_rreg(mmGRBM_SOFT_RESET) & ~bits);
    (void)gcn_rreg(mmGRBM_SOFT_RESET);
    gcn_wreg(mmGMCON_DEBUG, gcn_rreg(mmGMCON_DEBUG) &
             ~(GMCON_DEBUG_GFX_STALL | GMCON_DEBUG_GFX_CLEAR));
    udelay_(1000);

    srbm_select(MEC_ME, MEC_PIPE, MEC_QUEUE, 0);
    gcn_wreg(mmCP_HQD_DEQUEUE_REQUEST, 0);
    gcn_wreg(mmCP_HQD_PQ_RPTR, 0);
    gcn_wreg(mmCP_HQD_PQ_WPTR, 0);
    srbm_select(0, 0, 0, 0);
}

int gcn_gpu_init(const char *fw_dir, int force, FILE *log)
{
    const GCN_INFO *i = gcn_dev_info();

    g_log = log;
    g_wptr_valid = 0;
    g_last_seq = 0;
    if (i->FbApertureLen < 64u * 1024 * 1024)
        return fail("video memory aperture too small (%u MB)",
                    i->FbApertureLen >> 20);
    if (i->CursorOffset + 0x10000 > gcn_region_off() &&
        i->CursorOffset != 0)
        return fail("display driver memory overlaps the compute region");

    if (getenv("GCN4XP_INIT") != NULL)
        g_dbg = (unsigned)strtoul(getenv("GCN4XP_INIT"), NULL, 0);
    g_use_gfx = (g_dbg & 0x10) != 0;
    if (!force && gcn_gpu_is_up()) {
        if ((g_use_gfx ? ring_test() : mec_test()) == 0)
            return 0;
        logf_("engine does not respond, reinitialising\n");
    }
    logf_("initialising the GPU engine (flags %x)\n", g_dbg);
    if (!(g_dbg & 1))
        engine_reset();
    else
        cp_halt(1);
    mc_init();
    if (!(g_dbg & 2))
        core_init();
    if (!(g_dbg & 4) && rlc_start(fw_dir) != 0)
        return -1;
    if (g_use_gfx ? cp_start(fw_dir) != 0 : mec_start(fw_dir) != 0)
        return -1;
    logf_("engine is up\n");
    return 0;
}

void gcn_gpu_status(FILE *out)
{
    fprintf(out, "CP_ME_CNTL    %08x\n", gcn_rreg(mmCP_ME_CNTL));
    fprintf(out, "CP_MEC_CNTL   %08x\n", gcn_rreg(mmCP_MEC_CNTL));
    fprintf(out, "GRBM_STATUS   %08x  STATUS2 %08x\n", gcn_rreg(mmGRBM_STATUS),
            gcn_rreg(mmGRBM_STATUS2));
    fprintf(out, "CP_STAT       %08x  BUSY %08x\n", gcn_rreg(mmCP_STAT),
            gcn_rreg(mmCP_BUSY_STAT));
    fprintf(out, "CP_STALLED    %08x %08x %08x\n", gcn_rreg(mmCP_STALLED_STAT1),
            gcn_rreg(mmCP_STALLED_STAT2), gcn_rreg(mmCP_STALLED_STAT3));
    fprintf(out, "CP_RB0        base %08x cntl %08x rptr %x wptr %x\n",
            gcn_rreg(mmCP_RB0_BASE), gcn_rreg(mmCP_RB0_CNTL),
            gcn_rreg(mmCP_RB0_RPTR), gcn_rreg(mmCP_RB0_WPTR));
    fprintf(out, "CP headers    me %08x pfp %08x\n",
            gcn_rreg(mmCP_ME_HEADER_DUMP), gcn_rreg(mmCP_PFP_HEADER_DUMP));
    fprintf(out, "CP_IB1        %08x:%08x size %x\n", gcn_rreg(mmCP_IB1_BASE_HI),
            gcn_rreg(mmCP_IB1_BASE_LO), gcn_rreg(mmCP_IB1_BUFSZ));
    fprintf(out, "RLC           cntl %08x stat %08x gpm %08x\n",
            gcn_rreg(mmRLC_CNTL), gcn_rreg(mmRLC_STAT),
            gcn_rreg(mmRLC_GPM_STAT));
    fprintf(out, "SCRATCH       %08x %08x\n", gcn_rreg(mmSCRATCH_REG0),
            gcn_rreg(mmSCRATCH_REG1));
    fprintf(out, "VM            ctx0 %08x ctx1 %08x l1 %08x l2 %08x\n",
            gcn_rreg(mmVM_CONTEXT0_CNTL), gcn_rreg(mmVM_CONTEXT1_CNTL),
            gcn_rreg(mmMC_VM_MX_L1_TLB_CNTL), gcn_rreg(mmVM_L2_CNTL));
    fprintf(out, "VM faults     ctx0 %08x @%08x  ctx1 %08x @%08x\n",
            gcn_rreg(mmVM_CONTEXT0_PROTECTION_FAULT_STATUS),
            gcn_rreg(mmVM_CONTEXT0_PROTECTION_FAULT_ADDR),
            gcn_rreg(mmVM_CONTEXT1_PROTECTION_FAULT_STATUS),
            gcn_rreg(mmVM_CONTEXT1_PROTECTION_FAULT_ADDR));
    mec_status(out);
    fprintf(out, "engine        %s\n", gcn_gpu_is_up() ? "up" : "down");
}

void gcn_ring_submit(const u32 *pkt, u32 count)
{
    if (g_use_gfx)
        gcn_ring_emit(pkt, count);
    else
        mec_emit(pkt, count);
}

/* ---- asynchronous submission (compute queue only) ---------------------- */

u32 gcn_gpu_completed(void)
{
    return fence_value();
}

int gcn_gpu_wait(u32 seq, u32 timeout_ms)
{
    if (fence_wait(seq, timeout_ms) != 0)
        return fail("GPU did not finish within %u ms (fence %08x, expected "
                    "%08x, GRBM_STATUS %08x)", timeout_ms, fence_value(), seq,
                    gcn_rreg(mmGRBM_STATUS));
    return 0;
}

u32 gcn_gpu_submit(u64 ib_addr, u32 dwords, u32 vmid, int acquire)
{
    u32 pkt[32], n = 0, seq;

    if (!g_wptr_valid || g_last_seq == 0)
        g_last_seq = fence_value();
    /* keep the ring from wrapping onto packets the MEC has not read */
    if (g_last_seq - fence_value() > 256)
        fence_wait(g_last_seq - 128, 10000);
    seq = ++g_last_seq;
    if (acquire) {
        /* CPU writes went to memory past the GPU caches: drop stale lines */
        pkt[n++] = PACKET3(PACKET3_ACQUIRE_MEM, 5);
        pkt[n++] = COHER_TCL1_ACTION_ENA | COHER_TC_ACTION_ENA |
                   COHER_TC_WB_ACTION_ENA | COHER_SH_KCACHE_ACTION_ENA |
                   COHER_SH_ICACHE_ACTION_ENA;
        pkt[n++] = 0xffffffff;
        pkt[n++] = 0xff;
        pkt[n++] = 0;
        pkt[n++] = 0;
        pkt[n++] = 0x0a;
    }
    pkt[n++] = PACKET3(PACKET3_INDIRECT_BUFFER, 2);
    pkt[n++] = (u32)ib_addr & 0xfffffffc;
    pkt[n++] = (u32)(ib_addr >> 32) & 0xffff;
    pkt[n++] = dwords | INDIRECT_BUFFER_VALID | (vmid << 24);
    n += emit_fence(pkt + n, seq);
    mec_emit(pkt, n);
    return seq;
}

int gcn_run_ib(u64 ib_gpu_addr, u32 dwords, u32 vmid, u32 timeout_ms)
{
    u32 pkt[16];
    u32 n = 0;
    u32 seq;

    if (!g_use_gfx)
        return gcn_gpu_wait(gcn_gpu_submit(ib_gpu_addr, dwords, vmid, 1),
                            timeout_ms);
    seq = next_seq();

    pkt[n++] = PACKET3(PACKET3_INDIRECT_BUFFER, 2);
    pkt[n++] = (u32)ib_gpu_addr & 0xfffffffc;
    pkt[n++] = (u32)(ib_gpu_addr >> 32) & 0xffff;
    pkt[n++] = dwords | (vmid << 24);
    /* hold the micro engine until every compute wave has ended ... */
    pkt[n++] = PACKET3(PACKET3_EVENT_WRITE, 0);
    pkt[n++] = EVENT_TYPE(CS_PARTIAL_FLUSH) | EVENT_INDEX(4);
    /* ... then publish the sequence number */
    pkt[n++] = PACKET3(PACKET3_SET_UCONFIG_REG, 1);
    pkt[n++] = mmSCRATCH_REG1 - SET_UCONFIG_REG_START;
    pkt[n++] = seq;
    gcn_ring_emit(pkt, n);
    if (ring_wait(seq, timeout_ms) != 0)
        return fail("GPU did not finish within %u ms (CP_STAT %08x, "
                    "GRBM_STATUS %08x)", timeout_ms, gcn_rreg(mmCP_STAT),
                    gcn_rreg(mmGRBM_STATUS));
    return 0;
}
