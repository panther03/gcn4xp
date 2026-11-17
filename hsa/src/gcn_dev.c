/*
 * gcn_dev.c - device access through the display driver's escape interface.
 *
 * Video memory is reached through the MM_INDEX/MM_DATA register pair, which
 * needs nothing from the driver beyond register access and can address all
 * of VRAM, not just the part behind the PCI aperture.
 */
#include <stdio.h>
#include <string.h>
#include "gcn_dev.h"
#include "gcn_regs.h"

static HDC g_hdc;
static HANDLE g_mutex;
static GCN_INFO g_info;
static char g_err[160];
static u32 g_mm_hi = 0xffffffff; /* cached MM_INDEX_HI */
static u8 *g_map;                /* CPU view of [g_map_off, +g_map_len) */
static u64 g_map_off;
static u32 g_map_len;

const char *gcn_dev_error(void)
{
    return g_err;
}

const GCN_INFO *gcn_dev_info(void)
{
    return &g_info;
}

/*
 * A process started by a service (an SSH session for example) has no
 * access to the display until it joins the interactive window station.
 */
static void attach_interactive(void)
{
    HWINSTA ws = OpenWindowStationA("WinSta0", FALSE, MAXIMUM_ALLOWED);
    HDESK desk;

    if (ws == NULL || !SetProcessWindowStation(ws))
        return;
    desk = OpenDesktopA("Default", 0, FALSE, MAXIMUM_ALLOWED);
    if (desk != NULL)
        SetThreadDesktop(desk);
}

static int esc(ULONG op, ULONG a0, ULONG a1, void *out, int cb_out)
{
    GCN_ESC_HEADER hdr;

    hdr.Op = op;
    hdr.Arg0 = a0;
    hdr.Arg1 = a1;
    return ExtEscape(g_hdc, GCN_ESCAPE, sizeof(hdr), (LPCSTR)&hdr, cb_out,
                     (LPSTR)out);
}

int gcn_dev_open(void)
{
    ULONG q = GCN_ESCAPE;

    if (g_hdc != NULL)
        return 0;
    attach_interactive();
    g_hdc = CreateDCA("DISPLAY", NULL, NULL, NULL);
    if (g_hdc == NULL) {
        strcpy(g_err, "cannot open the display device");
        return -1;
    }
    if (ExtEscape(g_hdc, QUERYESCSUPPORT, sizeof(q), (LPCSTR)&q, 0, NULL) <=
        0 ||
        esc(GCN_ESC_INFO, 0, 0, &g_info, sizeof(g_info)) <
        (int)sizeof(g_info)) {
        strcpy(g_err, "the display is not driven by gcn4xp");
        gcn_dev_close();
        return -1;
    }
    if (!g_info.DebugBuild) {
        strcpy(g_err, "the installed gcn4xp driver has no register write "
                      "access (debug build needed)");
        gcn_dev_close();
        return -1;
    }
    g_mm_hi = 0xffffffff;
    return 0;
}

void gcn_dev_close(void)
{
    if (g_map != NULL)
        gcn_dev_unmap(g_map);
    if (g_hdc != NULL)
        DeleteDC(g_hdc);
    g_hdc = NULL;
}

void *gcn_dev_map(u64 offset, u32 len)
{
    void *va = NULL;

    if (g_hdc == NULL || g_map != NULL)
        return NULL;
    if (esc(GCN_ESC_MAP_VRAM, (ULONG)offset, len, &va, sizeof(va)) <
        (int)sizeof(va) || va == NULL) {
        snprintf(g_err, sizeof(g_err), "cannot map video memory %llx+%x",
                 (unsigned long long)offset, len);
        return NULL;
    }
    g_map = (u8 *)va;
    g_map_off = offset;
    g_map_len = len;
    return va;
}

void gcn_dev_unmap(void *va)
{
    if (va == NULL || g_hdc == NULL)
        return;
    esc(GCN_ESC_UNMAP_VRAM, (ULONG)(ULONG_PTR)va, 0, NULL, 0);
    if (va == g_map)
        g_map = NULL;
}

void *gcn_dev_ptr(u64 offset)
{
    if (g_map == NULL || offset < g_map_off || offset - g_map_off >= g_map_len)
        return NULL;
    return g_map + (offset - g_map_off);
}

void gcn_dev_flush_writes(void)
{
    /* drains the write-combining buffers, then the HDP write queue */
    __sync_synchronize();
    gcn_wreg(mmHDP_MEM_COHERENCY_FLUSH_CNTL, 0);
}

void gcn_dev_invalidate_reads(void)
{
    gcn_wreg(mmHDP_DEBUG0, 1);
    __sync_synchronize();
}

int gcn_dev_lock(DWORD timeout_ms)
{
    DWORD r;

    if (g_mutex == NULL) {
        SECURITY_DESCRIPTOR sd;
        SECURITY_ATTRIBUTES sa;

        /* NULL DACL: any session's processes share the one GPU. */
        InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = &sd;
        sa.bInheritHandle = FALSE;
        g_mutex = CreateMutexA(&sa, FALSE, "Global\\gcn4xp-gpu");
        if (g_mutex == NULL)
            g_mutex = CreateMutexA(&sa, FALSE, "gcn4xp-gpu");
        if (g_mutex == NULL) {
            strcpy(g_err, "cannot create the GPU lock");
            return -1;
        }
    }
    r = WaitForSingleObject(g_mutex, timeout_ms);
    if (r != WAIT_OBJECT_0 && r != WAIT_ABANDONED) {
        strcpy(g_err, "the GPU is in use by another process");
        return -1;
    }
    g_mm_hi = 0xffffffff; /* another process may have moved it */
    return 0;
}

void gcn_dev_unlock(void)
{
    if (g_mutex != NULL)
        ReleaseMutex(g_mutex);
}

u32 gcn_rreg(u32 index)
{
    u32 v = 0;

    esc(GCN_ESC_REG_READ, index, 1, &v, sizeof(v));
    return v;
}

void gcn_wreg(u32 index, u32 value)
{
    esc(GCN_ESC_REG_WRITE, index, value, NULL, 0);
}

static void mm_select(u64 offset)
{
    u32 hi = (u32)(offset >> 31);

    if (hi != g_mm_hi) {
        gcn_wreg(mmMM_INDEX_HI, hi);
        g_mm_hi = hi;
    }
    gcn_wreg(mmMM_INDEX, ((u32)offset & 0x7ffffffc) | 0x80000000u);
}

u32 gcn_vram_rd32(u64 offset)
{
    volatile u32 *p = (volatile u32 *)gcn_dev_ptr(offset & ~3ull);

    if (p != NULL)
        return *p;
    mm_select(offset);
    return gcn_rreg(mmMM_DATA);
}

void gcn_vram_wr32(u64 offset, u32 value)
{
    volatile u32 *p = (volatile u32 *)gcn_dev_ptr(offset & ~3ull);

    if (p != NULL) {
        *p = value;
        return;
    }
    mm_select(offset);
    gcn_wreg(mmMM_DATA, value);
}

/* the whole range inside the mapping? */
static u8 *mapped(u64 offset, u32 bytes)
{
    u8 *p = (u8 *)gcn_dev_ptr(offset);

    if (p == NULL || bytes == 0 || gcn_dev_ptr(offset + bytes - 1) == NULL)
        return NULL;
    return p;
}

void gcn_vram_read(u64 offset, void *buf, u32 bytes)
{
    u8 *p = (u8 *)buf;
    u8 *m = mapped(offset, bytes);

    if (m != NULL) {
        memcpy(buf, m, bytes);
        return;
    }

    while (bytes != 0) {
        u32 skip = (u32)offset & 3;
        u32 n = 4 - skip;
        u32 v = gcn_vram_rd32(offset - skip);

        if (n > bytes)
            n = bytes;
        memcpy(p, (u8 *)&v + skip, n);
        p += n;
        offset += n;
        bytes -= n;
    }
}

void gcn_vram_write(u64 offset, const void *buf, u32 bytes)
{
    const u8 *p = (const u8 *)buf;
    u8 *m = mapped(offset, bytes);

    if (m != NULL) {
        memcpy(m, buf, bytes);
        return;
    }
    while (bytes != 0) {
        u32 skip = (u32)offset & 3;
        u32 n = 4 - skip;
        u32 v = 0;

        if (n > bytes)
            n = bytes;
        if (n != 4)
            v = gcn_vram_rd32(offset - skip);
        memcpy((u8 *)&v + skip, p, n);
        gcn_vram_wr32(offset - skip, v);
        p += n;
        offset += n;
        bytes -= n;
    }
}

void gcn_vram_fill(u64 offset, u32 value, u32 bytes)
{
    u32 i;
    u32 *m = (u32 *)mapped(offset, bytes);

    if (m != NULL) {
        for (i = 0; i < bytes / 4; i++)
            m[i] = value;
        return;
    }
    for (i = 0; i < bytes; i += 4)
        gcn_vram_wr32(offset + i, value);
}
