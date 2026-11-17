/*
 * gcngpu - brings up and tests the GPU compute engine.
 *
 *   gcngpu init [-f]   start the engine (-f: reset and start again)
 *   gcngpu status      engine registers
 *   gcngpu cstest      run a tiny compute shader without address
 *                      translation and check what it stored
 *   gcngpu vmtest      the same through the user address space (VMID 1)
 *   gcngpu bench       time register and video memory access
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "gcn_gpu.h"
#include "gcn_regs.h"

/* v0 = 0x12345678; store it at the address in s[0:1] */
static const u32 g_store_shader[] = {
    0x7e0002ff, 0x12345678, /* v_mov_b32 v0, 0x12345678 */
    0x7e020200,             /* v_mov_b32 v1, s0 */
    0x7e040201,             /* v_mov_b32 v2, s1 */
    0xdc700000, 0x00000001, /* flat_store_dword v[1:2], v0 */
    0xbf8c0000,             /* s_waitcnt 0 */
    0xbf810000,             /* s_endpgm */
};

static int cstest(void)
{
    u64 base = gcn_region_off() + GCN_TEST_OFF;
    u64 code = base + 0x1000, data = base + 0x2000;
    u64 code_mc = gcn_mc_addr(code), data_mc = gcn_mc_addr(data);
    u32 ib[64], n = 0, i, got;

    gcn_vram_fill(base, 0, 0x1000);
    gcn_vram_write(code, g_store_shader, sizeof(g_store_shader));
    gcn_vram_wr32(data, 0xdeadbeef);

#define SH(reg, cnt) \
    ib[n++] = PACKET3(PACKET3_SET_SH_REG, cnt); \
    ib[n++] = (reg) - SET_SH_REG_START
    SH(mmCOMPUTE_STATIC_THREAD_MGMT_SE0, 2);
    ib[n++] = 0xffffffff;
    ib[n++] = 0xffffffff;
    SH(mmCOMPUTE_STATIC_THREAD_MGMT_SE2, 2);
    ib[n++] = 0xffffffff;
    ib[n++] = 0xffffffff;
    SH(mmCOMPUTE_RESOURCE_LIMITS, 1);
    ib[n++] = 0;
    SH(mmCOMPUTE_START_X, 3);
    ib[n++] = 0;
    ib[n++] = 0;
    ib[n++] = 0;
    SH(mmCOMPUTE_NUM_THREAD_X, 3);
    ib[n++] = 1;
    ib[n++] = 1;
    ib[n++] = 1;
    SH(mmCOMPUTE_PGM_LO, 2);
    ib[n++] = (u32)(code_mc >> 8);
    ib[n++] = (u32)(code_mc >> 40);
    SH(mmCOMPUTE_PGM_RSRC1, 2);
    ib[n++] = 0x002c0040;           /* 8 vgprs, 16 sgprs, default float mode */
    ib[n++] = RSRC2_USER_SGPR(2);
    SH(mmCOMPUTE_TMPRING_SIZE, 1);
    ib[n++] = 0;
    SH(mmCOMPUTE_USER_DATA_0, 2);
    ib[n++] = (u32)data_mc;
    ib[n++] = (u32)(data_mc >> 32);
    ib[n++] = PACKET3_COMPUTE(PACKET3_DISPATCH_DIRECT, 3);
    ib[n++] = 1;
    ib[n++] = 1;
    ib[n++] = 1;
    ib[n++] = COMPUTE_SHADER_EN | FORCE_START_AT_000;
    for (i = 0; i < n; i++)
        gcn_vram_wr32(base + i * 4, ib[i]);

    if (gcn_run_ib(gcn_mc_addr(base), n, 0, 3000) != 0) {
        printf("dispatch failed: %s\n", gcn_gpu_error());
        gcn_gpu_status(stdout);
        return 1;
    }
    got = gcn_vram_rd32(data);
    printf("shader stored %08x (%s)\n", got,
           got == 0x12345678 ? "correct" : "WRONG, expected 12345678");
    return got == 0x12345678 ? 0 : 1;
}

/*
 * Same shader, but through the user address space: the compute region is
 * mapped into this process and VMID 1 maps the same virtual addresses to
 * the same memory, so the shader stores through the CPU's own pointer.
 */
static int vmtest(void)
{
    u8 *cpu = (u8 *)gcn_dev_ptr(gcn_region_off());
    u64 heap_va;
    u32 *code, *ib, *data, n = 0;

    if (cpu == NULL) {
        printf("video memory is not mapped: %s\n", gcn_dev_error());
        return 1;
    }
    heap_va = (u64)(ULONG_PTR)(cpu + GCN_HEAP_OFF);
    gcn_vm_map(heap_va, (u32)(gcn_heap_bytes() / GCN_GPU_PAGE), 0);
    code = (u32 *)(cpu + GCN_HEAP_OFF + 0x1000);
    ib = (u32 *)(cpu + GCN_HEAP_OFF + 0x2000);
    data = (u32 *)(cpu + GCN_HEAP_OFF + 0x3000);
    memcpy(code, g_store_shader, sizeof(g_store_shader));
    *data = 0xdeadbeef;

#define SHV(reg, cnt) \
    ib[n++] = PACKET3(PACKET3_SET_SH_REG, cnt); \
    ib[n++] = (reg) - SET_SH_REG_START
    SHV(mmCOMPUTE_STATIC_THREAD_MGMT_SE0, 2);
    ib[n++] = 0xffffffff;
    ib[n++] = 0xffffffff;
    SHV(mmCOMPUTE_STATIC_THREAD_MGMT_SE2, 2);
    ib[n++] = 0xffffffff;
    ib[n++] = 0xffffffff;
    SHV(mmCOMPUTE_RESOURCE_LIMITS, 1);
    ib[n++] = 0;
    SHV(mmCOMPUTE_START_X, 3);
    ib[n++] = 0;
    ib[n++] = 0;
    ib[n++] = 0;
    SHV(mmCOMPUTE_NUM_THREAD_X, 3);
    ib[n++] = 1;
    ib[n++] = 1;
    ib[n++] = 1;
    SHV(mmCOMPUTE_PGM_LO, 2);
    ib[n++] = (u32)((ULONG_PTR)code >> 8);
    ib[n++] = 0;
    SHV(mmCOMPUTE_PGM_RSRC1, 2);
    ib[n++] = 0x002c0040;
    ib[n++] = RSRC2_USER_SGPR(2);
    SHV(mmCOMPUTE_TMPRING_SIZE, 1);
    ib[n++] = 0;
    SHV(mmCOMPUTE_USER_DATA_0, 2);
    ib[n++] = (u32)(ULONG_PTR)data;
    ib[n++] = 0;
    ib[n++] = PACKET3_COMPUTE(PACKET3_DISPATCH_DIRECT, 3);
    ib[n++] = 1;
    ib[n++] = 1;
    ib[n++] = 1;
    ib[n++] = COMPUTE_SHADER_EN | FORCE_START_AT_000;

    gcn_dev_flush_writes();
    if (gcn_run_ib((u64)(ULONG_PTR)ib, n, GCN_USER_VMID, 3000) != 0) {
        printf("dispatch failed: %s\n", gcn_gpu_error());
        gcn_vm_fault(stdout);
        gcn_gpu_status(stdout);
        return 1;
    }
    gcn_dev_invalidate_reads();
    printf("shader stored %08x at %p (%s)\n", *data, (void *)data,
           *data == 0x12345678 ? "correct" : "WRONG, expected 12345678");
    if (gcn_vm_fault(stdout))
        return 1;
    return *data == 0x12345678 ? 0 : 1;
}

/*
 * CPU reads of video memory through the mapping, at each access width.
 * Each width reads its own freshly written area (a pattern that differs
 * from run to run), so stale data cached anywhere on the way shows up:
 * once right after the writes, once after dropping the HDP read cache.
 */
#include <emmintrin.h>

static u32 pattern(u32 seed, u32 i)
{
    return (seed ^ (i * 0x9e3779b1u)) + i;
}

static u32 read_check(volatile u32 *p, u32 words, int width, u32 seed)
{
    static u32 buf[4096];
    u32 i, j, bad = 0;

    if (width == 0) {
        memcpy(buf, (const void *)p, words * 4);
        for (i = 0; i < words; i++)
            bad += buf[i] != pattern(seed, i);
        return bad;
    }
    for (i = 0; i < words; i += width / 4) {
        u32 w[4];

        if (width == 4)
            w[0] = p[i];
        else if (width == 8)
            _mm_storeu_si128((__m128i *)w,
                             _mm_loadl_epi64((const __m128i *)(p + i)));
        else
            _mm_storeu_si128((__m128i *)w,
                             _mm_loadu_si128((const __m128i *)(p + i)));
        for (j = 0; j < (u32)width / 4; j++)
            bad += w[j] != pattern(seed, i + j);
    }
    return bad;
}

static int memtest(void)
{
    enum { WORDS = 4096 };
    static const int widths[4] = { 4, 8, 16, 0 };
    static const char *const names[4] = { "32-bit", "64-bit", "128-bit",
                                          "memcpy" };
    volatile u32 *base = (volatile u32 *)gcn_dev_ptr(gcn_region_off() +
                                                     GCN_TEST_OFF + 0x8000);
    u32 seed = GetTickCount() * 2654435761u, total = 0, i;
    int inval, k;

    if (base == NULL) {
        printf("video memory is not mapped: %s\n", gcn_dev_error());
        return 1;
    }
    for (inval = 0; inval < 2; inval++) {
        printf("%s:", inval ? "after HDP read cache drop" : "right after writing");
        for (k = 0; k < 4; k++) {
            volatile u32 *p = base + k * WORDS;
            u32 s = seed + (u32)(inval * 4 + k), bad;

            for (i = 0; i < WORDS; i++)
                p[i] = pattern(s, i);
            gcn_dev_flush_writes();
            if (inval)
                gcn_dev_invalidate_reads();
            bad = read_check(p, WORDS, widths[k], s);
            total += bad;
            printf("  %s %u", names[k], bad);
        }
        printf("  (wrong words of %u)\n", WORDS);
    }
    return total != 0;
}

static int bench(void)
{
    DWORD t = GetTickCount();
    u64 off = gcn_region_off() + GCN_TEST_OFF + 0x4000;
    u32 i, bad = 0;

    for (i = 0; i < 20000; i++)
        (void)gcn_rreg(mmGRBM_STATUS);
    printf("register read:  %u us\n", (unsigned)(GetTickCount() - t) / 20);
    t = GetTickCount();
    for (i = 0; i < 4096; i++)
        gcn_vram_wr32(off + i * 4, i * 0x01010101u);
    for (i = 0; i < 4096; i++)
        if (gcn_vram_rd32(off + i * 4) != i * 0x01010101u)
            bad++;
    printf("vram word r+w:  %u us, %u mismatches\n",
           (unsigned)((GetTickCount() - t) * 1000 / 4096), bad);
    return bad != 0;
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "";
    int r = 0;

    if (gcn_dev_open() != 0) {
        fprintf(stderr, "gcngpu: %s\n", gcn_dev_error());
        return 2;
    }
    if (gcn_dev_lock(5000) != 0) {
        fprintf(stderr, "gcngpu: %s\n", gcn_dev_error());
        return 2;
    }
    /* the fast path; everything still works through registers without it */
    if (getenv("GCN4XP_NOMAP") == NULL &&
        gcn_dev_map(gcn_region_off(), (u32)(gcn_dev_info()->FbApertureLen -
                                            gcn_region_off())) == NULL)
        fprintf(stderr, "gcngpu: %s\n", gcn_dev_error());
    if (strcmp(cmd, "init") == 0) {
        int force = argc > 2 && strcmp(argv[2], "-f") == 0;
        r = gcn_gpu_init(gcn_firmware_dir(), force, stdout) != 0;
        if (r)
            gcn_gpu_status(stdout);
    } else if (strcmp(cmd, "status") == 0) {
        gcn_gpu_status(stdout);
    } else if (strcmp(cmd, "cstest") == 0) {
        r = gcn_gpu_init(gcn_firmware_dir(), 0, stdout) != 0 || cstest();
    } else if (strcmp(cmd, "vmtest") == 0) {
        r = gcn_gpu_init(gcn_firmware_dir(), 0, stdout) != 0 || vmtest();
    } else if (strcmp(cmd, "memtest") == 0) {
        r = memtest();
    } else if (strcmp(cmd, "bench") == 0) {
        r = bench();
    } else {
        fprintf(stderr, "usage: gcngpu init [-f] | status | cstest | vmtest | "
                        "bench\n");
        r = 2;
    }
    gcn_dev_unlock();
    gcn_dev_close();
    return r;
}
