/*
 * gcnmp.c - video miniport for AMD GCN (Polaris / DCE 11.2) cards on
 * Windows XP x86.
 *
 * Responsibilities:
 *   - claim the PCI resources and map the register BAR
 *   - enumerate and set display modes through the card's VESA BIOS
 *   - map the linear framebuffer write-combined for the display driver
 *   - drive the display controller's hardware cursor
 *   - provide a pageable shadow buffer and diagnostics to the display driver
 */
#include <stdarg.h>
#include <intrin.h>
#include "vp.h"
#include "gcnreg.h"
#include "gcn_shared.h"

#define GCN_MAX_MODES   96
#define GCN_LOG_SIZE    8192
#define GCN_POOL_TAG    'pNCG'

typedef struct _GCN_MODE {
    USHORT VbeMode;
    USHORT Width;
    USHORT Height;
    USHORT Pitch;
    UCHAR  Bpp;
    UCHAR  RedSize, RedPos;
    UCHAR  GreenSize, GreenPos;
    UCHAR  BlueSize, BluePos;
    ULONG  PhysBase;
} GCN_MODE;

typedef struct _GCN_EXT {
    volatile ULONG *Mmio;
    ULONG  MmioLen;
    PHYSICAL_ADDRESS MmioPhys;
    PHYSICAL_ADDRESS FbPhys;
    ULONG  FbLen;
    USHORT VendorId;
    USHORT DeviceId;
    UCHAR  Revision;
    ULONG  VramMB;

    VIDEO_PORT_INT10_INTERFACE Int10;
    BOOLEAN Int10Ok;
    BOOLEAN ModesEnumerated;
    USHORT VbeVersion;
    USHORT ModeHz[GCN_MAX_MODES]; /* measured refresh rate, 0 = not yet */
    ULONG  DefaultHz;             /* last rate measured on any mode */
    ULONG  NumModes;
    LONG   CurrentMode;
    GCN_MODE Modes[GCN_MAX_MODES];

    PVOID  FbVa;
    ULONG  FbMapLen;
    BOOLEAN WriteCombined;

    ULONG  CrtcMask;
    ULONG  McFbBaseLow;
    ULONG  McFbBaseHigh;
    PULONG CursorVa;
    ULONG  CursorOffset;
    BOOLEAN CursorOk;       /* usable with the current mode */
    BOOLEAN CursorShown;

    ULONG  LogPos;
    CHAR   Log[GCN_LOG_SIZE];
} GCN_EXT, *PGCN_EXT;

static const USHORT g_CrtcOffsets[GCN_MAX_CRTC] = GCN_CRTC_OFFSETS;

/* ---- small helpers ------------------------------------------------------ */

static __inline ULONG RREG(PGCN_EXT Ext, ULONG Index)
{
    return Ext->Mmio[Index];
}

static __inline VOID WREG(PGCN_EXT Ext, ULONG Index, ULONG Value)
{
    Ext->Mmio[Index] = Value;
}

static VOID LogChar(PGCN_EXT Ext, CHAR c)
{
    Ext->Log[Ext->LogPos % GCN_LOG_SIZE] = c;
    Ext->LogPos++;
}

static VOID LogNum(PGCN_EXT Ext, ULONG Value, ULONG Base, ULONG MinDigits)
{
    CHAR tmp[12];
    ULONG n = 0;

    do {
        ULONG d = Value % Base;
        tmp[n++] = (CHAR)(d < 10 ? '0' + d : 'a' + d - 10);
        Value /= Base;
    } while (Value != 0 && n < sizeof(tmp));
    while (MinDigits > n) {
        LogChar(Ext, '0');
        MinDigits--;
    }
    while (n > 0)
        LogChar(Ext, tmp[--n]);
}

/* Minimal printf: %u %d %x %s %c, optional zero-padded width (%08x). */
static VOID KLog(PGCN_EXT Ext, const CHAR *Fmt, ...)
{
    va_list ap;

    va_start(ap, Fmt);
    for (; *Fmt; Fmt++) {
        ULONG width = 0;

        if (*Fmt != '%') {
            LogChar(Ext, *Fmt);
            continue;
        }
        Fmt++;
        while (*Fmt >= '0' && *Fmt <= '9')
            width = width * 10 + (ULONG)(*Fmt++ - '0');
        switch (*Fmt) {
        case 'u':
            LogNum(Ext, va_arg(ap, ULONG), 10, width);
            break;
        case 'd': {
            LONG v = va_arg(ap, LONG);
            if (v < 0) {
                LogChar(Ext, '-');
                v = -v;
            }
            LogNum(Ext, (ULONG)v, 10, width);
            break;
        }
        case 'x':
            LogNum(Ext, va_arg(ap, ULONG), 16, width);
            break;
        case 's': {
            const CHAR *s = va_arg(ap, const CHAR *);
            while (*s)
                LogChar(Ext, *s++);
            break;
        }
        case 'c':
            LogChar(Ext, (CHAR)va_arg(ap, int));
            break;
        case 0:
            Fmt--;
            break;
        default:
            LogChar(Ext, *Fmt);
            break;
        }
    }
    va_end(ap);
}

static ULONG WStrBytes(const WCHAR *s)
{
    ULONG n = 0;
    while (s[n])
        n++;
    return (n + 1) * sizeof(WCHAR);
}

static VOID SetRegString(PGCN_EXT Ext, PWSTR Name, PWSTR Value)
{
    VideoPortSetRegistryParameters(Ext, Name, Value, WStrBytes(Value));
}

/*
 * Debug builds also write diagnostics directly under the service key
 * (HKLM\SYSTEM\CurrentControlSet\Services\gcnmp), which works before the
 * video port has set anything up.
 */
#ifdef GCN_DEBUG
LONG NTAPI RtlWriteRegistryValue(ULONG RelativeTo, const WCHAR *Path,
                                 const WCHAR *ValueName, ULONG ValueType,
                                 PVOID ValueData, ULONG ValueLength);

static VOID DbgReg(const WCHAR *Name, ULONG Type, PVOID Data, ULONG Length)
{
    RtlWriteRegistryValue(1 /* RTL_REGISTRY_SERVICES */, L"gcnmp", Name, Type,
                          Data, Length);
}
#else
#define DbgReg(Name, Type, Data, Length) ((void)0)
#endif

/*
 * Mirrors the log into the adapter's registry key (value "gcn.Log") so it
 * can be read even when the display driver never comes up.  PASSIVE_LEVEL
 * only.
 */
static VOID LogSave(PGCN_EXT Ext)
{
    ULONG n = Ext->LogPos < GCN_LOG_SIZE ? Ext->LogPos : GCN_LOG_SIZE;

    /* Only meaningful until the ring wraps; that is all bring-up needs. */
    if (n != 0) {
        VideoPortSetRegistryParameters(Ext, L"gcn.Log", Ext->Log, n);
        DbgReg(L"DebugLog", REG_BINARY, Ext->Log, n);
    }
}

/* ---- VESA BIOS access --------------------------------------------------- */

static BOOLEAN Int10Acquire(PGCN_EXT Ext)
{
    if (Ext->Int10Ok)
        return TRUE;
    Ext->Int10.Size = sizeof(Ext->Int10);
    Ext->Int10.Version = 1;
    if (VideoPortQueryServices(Ext, VideoPortServicesInt10, &Ext->Int10) !=
        NO_ERROR) {
        KLog(Ext, "int10: service not available\n");
        LogSave(Ext);
        return FALSE;
    }
    Ext->Int10Ok = TRUE;
    return TRUE;
}

/* Runs a VBE function; TRUE when the BIOS reports success (AX = 004Fh). */
static BOOLEAN VbeCall(PGCN_EXT Ext, PINT10_BIOS_ARGUMENTS Args)
{
    if (Ext->Int10.Int10CallBios(Ext->Int10.Context, Args) != NO_ERROR)
        return FALSE;
    return (Args->Eax & 0xFFFF) == 0x004F;
}

static USHORT RdW(const UCHAR *p, ULONG off)
{
    return (USHORT)(p[off] | (p[off + 1] << 8));
}

static ULONG RdD(const UCHAR *p, ULONG off)
{
    return (ULONG)p[off] | ((ULONG)p[off + 1] << 8) |
           ((ULONG)p[off + 2] << 16) | ((ULONG)p[off + 3] << 24);
}

static VOID VbeAddMode(PGCN_EXT Ext, USHORT Number, const UCHAR *mi)
{
    GCN_MODE *m;
    USHORT attrs = RdW(mi, 0x00);
    BOOLEAN vbe3 = Ext->VbeVersion >= 0x0300;
    ULONG i;

    /* supported, colour, graphics, linear framebuffer available */
    if ((attrs & 0x0099) != 0x0099)
        return;
    if (mi[0x1B] != 6)               /* direct colour memory model */
        return;
    if (mi[0x19] != 32 && mi[0x19] != 16)
        return;
    if (RdD(mi, 0x28) == 0)
        return;
    if (Ext->NumModes >= GCN_MAX_MODES)
        return;

    m = &Ext->Modes[Ext->NumModes];
    m->VbeMode = Number;
    m->Width = RdW(mi, 0x12);
    m->Height = RdW(mi, 0x14);
    m->Bpp = mi[0x19];
    m->PhysBase = RdD(mi, 0x28);
    if (vbe3 && RdW(mi, 0x32) != 0) {
        m->Pitch = RdW(mi, 0x32);
        m->RedSize = mi[0x36];   m->RedPos = mi[0x37];
        m->GreenSize = mi[0x38]; m->GreenPos = mi[0x39];
        m->BlueSize = mi[0x3A];  m->BluePos = mi[0x3B];
    } else {
        m->Pitch = RdW(mi, 0x10);
        m->RedSize = mi[0x1F];   m->RedPos = mi[0x20];
        m->GreenSize = mi[0x21]; m->GreenPos = mi[0x22];
        m->BlueSize = mi[0x23];  m->BluePos = mi[0x24];
    }
    if (m->Width == 0 || m->Height == 0 ||
        m->Pitch < (ULONG)m->Width * (m->Bpp / 8))
        return;
    /* the framebuffer must lie inside the aperture we own */
    if (Ext->FbPhys.HighPart != 0 || m->PhysBase < Ext->FbPhys.LowPart ||
        m->PhysBase - Ext->FbPhys.LowPart + (ULONG)m->Pitch * m->Height >
            Ext->FbLen)
        return;
    for (i = 0; i < Ext->NumModes; i++) {
        if (Ext->Modes[i].Width == m->Width &&
            Ext->Modes[i].Height == m->Height && Ext->Modes[i].Bpp == m->Bpp)
            return;
    }
    Ext->NumModes++;
    KLog(Ext, "vbe: mode %x %ux%ux%u pitch %u\n", (ULONG)Number,
         (ULONG)m->Width, (ULONG)m->Height, (ULONG)m->Bpp, (ULONG)m->Pitch);
}

/*
 * Builds the mode table from the VESA BIOS.  Needs a context in which the
 * video port can run real-mode BIOS code, which is not the case during
 * early boot, so this is called lazily and retried until it succeeds.
 */
static BOOLEAN VbeEnumerate(PGCN_EXT Ext)
{
    INT10_BIOS_ARGUMENTS a;
    USHORT seg = 0, off = 0, list[160];
    ULONG len = 0x400, count = 0, i;
    UCHAR blk[512];
    BOOLEAN ok = FALSE, borrowed = FALSE;

    if (Ext->ModesEnumerated)
        return TRUE;
    if (!Int10Acquire(Ext))
        return FALSE;
    if (Ext->Int10.Int10AllocateBuffer(Ext->Int10.Context, &seg, &off,
                                       &len) != NO_ERROR) {
        /*
         * The XP video port has exactly one real-mode transfer buffer, at
         * 2000:0000, and vga.sys takes it at boot and never releases it.
         * vga.sys is idle while this driver owns the display, so borrow
         * the buffer rather than fail.
         */
        KLog(Ext, "vbe: transfer buffer busy, borrowing 2000:0000\n");
        seg = 0x2000;
        off = 0;
        borrowed = TRUE;
    }

    memset(blk, 0, sizeof(blk));
    blk[0] = 'V'; blk[1] = 'B'; blk[2] = 'E'; blk[3] = '2';
    Ext->Int10.Int10WriteMemory(Ext->Int10.Context, seg, off, blk, 512);
    memset(&a, 0, sizeof(a));
    a.Eax = 0x4F00;
    a.Edi = off;
    a.SegEs = seg;
    if (!VbeCall(Ext, &a)) {
        KLog(Ext, "vbe: controller info failed, ax=%x\n", a.Eax);
        goto done;
    }
    Ext->Int10.Int10ReadMemory(Ext->Int10.Context, seg, off, blk, 512);
    if (blk[0] != 'V' || blk[1] != 'E' || blk[2] != 'S' || blk[3] != 'A') {
        KLog(Ext, "vbe: bad signature\n");
        goto done;
    }
    Ext->VbeVersion = RdW(blk, 0x04);
    KLog(Ext, "vbe: version %x, %u KB\n", (ULONG)Ext->VbeVersion,
         (ULONG)RdW(blk, 0x12) * 64);

    /* Copy the mode list first: it may live in the buffer reused below. */
    for (count = 0; count < 160; count++) {
        USHORT m = 0xFFFF;
        Ext->Int10.Int10ReadMemory(Ext->Int10.Context, RdW(blk, 0x10),
                                   (USHORT)(RdW(blk, 0x0E) + count * 2), &m,
                                   2);
        if (m == 0xFFFF)
            break;
        list[count] = m;
    }

    Ext->NumModes = 0;
    for (i = 0; i < count; i++) {
        memset(blk, 0, 256);
        Ext->Int10.Int10WriteMemory(Ext->Int10.Context, seg, off, blk, 256);
        memset(&a, 0, sizeof(a));
        a.Eax = 0x4F01;
        a.Ecx = list[i];
        a.Edi = off;
        a.SegEs = seg;
        if (!VbeCall(Ext, &a))
            continue;
        Ext->Int10.Int10ReadMemory(Ext->Int10.Context, seg, off, blk, 256);
        VbeAddMode(Ext, list[i], blk);
    }
    KLog(Ext, "vbe: %u of %u modes usable\n", Ext->NumModes, count);
    ok = Ext->NumModes != 0;
    Ext->ModesEnumerated = ok;

done:
    if (!borrowed)
        Ext->Int10.Int10FreeBuffer(Ext->Int10.Context, seg, off);
    LogSave(Ext);
    return ok;
}

/* ---- display controller ------------------------------------------------- */

static VOID CursorHide(PGCN_EXT Ext)
{
    ULONG i;

    if (Ext->Mmio == NULL)
        return;
    for (i = 0; i < GCN_MAX_CRTC; i++) {
        ULONG o = g_CrtcOffsets[i], ctl;

        if (!(Ext->CrtcMask & (1u << i)))
            continue;
        ctl = RREG(Ext, mmCUR_CONTROL + o);
        if (ctl & CUR_CONTROL__CURSOR_EN_MASK) {
            WREG(Ext, mmCUR_UPDATE + o,
                 RREG(Ext, mmCUR_UPDATE + o) |
                     CUR_UPDATE__CURSOR_UPDATE_LOCK_MASK);
            WREG(Ext, mmCUR_CONTROL + o, ctl & ~CUR_CONTROL__CURSOR_EN_MASK);
            WREG(Ext, mmCUR_UPDATE + o,
                 RREG(Ext, mmCUR_UPDATE + o) &
                     ~CUR_UPDATE__CURSOR_UPDATE_LOCK_MASK);
        }
    }
    Ext->CursorShown = FALSE;
}

static VOID CursorSet(PGCN_EXT Ext, const GCN_POINTER_POS *Pos)
{
    LONG x = Pos->X - (LONG)Pos->HotX;
    LONG y = Pos->Y - (LONG)Pos->HotY;
    ULONG xorg = 0, yorg = 0, addrLow, addrHigh, i;

    if (!Ext->CursorOk || !Pos->Visible || x <= -GCN_CURSOR_HW_DIM ||
        y <= -GCN_CURSOR_HW_DIM) {
        CursorHide(Ext);
        return;
    }
    /* Off the top/left edge: keep the position at 0 and shift the image. */
    if (x < 0) {
        xorg = (ULONG)(-x);
        x = 0;
    }
    if (y < 0) {
        yorg = (ULONG)(-y);
        y = 0;
    }
    addrLow = Ext->McFbBaseLow + Ext->CursorOffset;
    addrHigh = Ext->McFbBaseHigh + (addrLow < Ext->McFbBaseLow ? 1 : 0);

    for (i = 0; i < GCN_MAX_CRTC; i++) {
        ULONG o = g_CrtcOffsets[i], ctl;

        if (!(Ext->CrtcMask & (1u << i)))
            continue;
        WREG(Ext, mmCUR_UPDATE + o,
             RREG(Ext, mmCUR_UPDATE + o) |
                 CUR_UPDATE__CURSOR_UPDATE_LOCK_MASK);
        WREG(Ext, mmCUR_SURFACE_ADDRESS_HIGH + o, addrHigh);
        WREG(Ext, mmCUR_SURFACE_ADDRESS + o, addrLow);
        WREG(Ext, mmCUR_SIZE + o,
             ((GCN_CURSOR_HW_DIM - 1) << 16) | (GCN_CURSOR_HW_DIM - 1));
        WREG(Ext, mmCUR_POSITION + o, ((ULONG)x << 16) | (ULONG)y);
        WREG(Ext, mmCUR_HOT_SPOT + o, (xorg << 16) | yorg);
        ctl = RREG(Ext, mmCUR_CONTROL + o);
        ctl &= ~(CUR_CONTROL__CURSOR_MODE_MASK |
                 CUR_CONTROL__CURSOR_2X_MAGNIFY_MASK);
        ctl |= CUR_CONTROL__CURSOR_EN_MASK |
               (CURSOR_MODE_24_8_PREMULT << CUR_CONTROL__CURSOR_MODE__SHIFT);
        WREG(Ext, mmCUR_CONTROL + o, ctl);
        WREG(Ext, mmCUR_UPDATE + o,
             RREG(Ext, mmCUR_UPDATE + o) &
                 ~CUR_UPDATE__CURSOR_UPDATE_LOCK_MASK);
    }
    Ext->CursorShown = TRUE;
}

/*
 * After a BIOS mode set: find which CRTCs scan out, and check that the
 * scanout address agrees with our model of the address space (VRAM offset
 * == aperture offset).  The hardware cursor is only used when it does.
 */
static VOID DceProbe(PGCN_EXT Ext, const GCN_MODE *Mode)
{
    ULONG loc = RREG(Ext, mmMC_VM_FB_LOCATION);
    ULONG fbOffset = Mode->PhysBase - Ext->FbPhys.LowPart;
    ULONG fbBytes = (ULONG)Mode->Pitch * Mode->Height;
    BOOLEAN match = FALSE, scaled = FALSE;
    ULONG i;

    Ext->McFbBaseLow = (loc & 0xFFFF) << 24;
    Ext->McFbBaseHigh = (loc & 0xFFFF) >> 8;
    Ext->CrtcMask = 0;

    for (i = 0; i < GCN_MAX_CRTC; i++) {
        ULONG o = g_CrtcOffsets[i];
        ULONG lo, hi;

        if (!(RREG(Ext, mmCRTC_CONTROL + o) &
              CRTC_CONTROL__CRTC_MASTER_EN_MASK))
            continue;
        if (!(RREG(Ext, mmGRPH_ENABLE + o) & GRPH_ENABLE__GRPH_ENABLE_MASK))
            continue;
        Ext->CrtcMask |= 1u << i;
        lo = RREG(Ext, mmGRPH_PRIMARY_SURFACE_ADDRESS + o) & 0xFFFFFF00;
        hi = RREG(Ext, mmGRPH_PRIMARY_SURFACE_ADDRESS_HIGH + o) & 0xFF;
        KLog(Ext, "dce: crtc %u surface %02x%08x pitch %u end %ux%u\n", i, hi,
             lo, RREG(Ext, mmGRPH_PITCH + o), RREG(Ext, mmGRPH_X_END + o),
             RREG(Ext, mmGRPH_Y_END + o));
        if (lo == Ext->McFbBaseLow + fbOffset &&
            hi == Ext->McFbBaseHigh +
                      (lo < Ext->McFbBaseLow ? 1u : 0u))
            match = TRUE;

        /*
         * The BIOS may drive the monitor at another resolution and let the
         * scaler stretch the framebuffer.  The cursor plane is not part of
         * the scaled image, so its size and position would be wrong.
         */
        {
            ULONG htotal = (RREG(Ext, mmCRTC_H_TOTAL + o) & 0x3FFF) + 1;
            ULONG vtotal = (RREG(Ext, mmCRTC_V_TOTAL + o) & 0x3FFF) + 1;
            ULONG hb = RREG(Ext, mmCRTC_H_BLANK_START_END + o);
            ULONG vb = RREG(Ext, mmCRTC_V_BLANK_START_END + o);
            ULONG hact = ((hb & 0x3FFF) + htotal - ((hb >> 16) & 0x3FFF)) %
                         htotal;
            ULONG vact = ((vb & 0x3FFF) + vtotal - ((vb >> 16) & 0x3FFF)) %
                         vtotal;
            ULONG vp = RREG(Ext, mmVIEWPORT_SIZE + o);

            KLog(Ext, "dce: crtc %u timing %ux%u viewport %ux%u scl %x\n", i,
                 hact, vact, (vp >> 16) & 0x3FFF, vp & 0x3FFF,
                 RREG(Ext, mmSCL_MODE + o));
            if (hact != Mode->Width || vact != Mode->Height)
                scaled = TRUE;
        }
    }
    KLog(Ext, "dce: fb base %02x%08x, crtc mask %x, layout %s\n",
         Ext->McFbBaseHigh, Ext->McFbBaseLow, Ext->CrtcMask,
         match ? "ok" : "unexpected");
    if (scaled)
        KLog(Ext, "dce: framebuffer is scaled, using software cursor\n");

    Ext->CursorOk = match && !scaled && Ext->CursorVa != NULL &&
                    fbOffset + fbBytes <= Ext->CursorOffset;
    Ext->CursorShown = FALSE;
}

/* ---- mode handling ------------------------------------------------------ */

static ULONG MaskOf(UCHAR Size, UCHAR Pos)
{
    return ((1u << Size) - 1) << Pos;
}

/*
 * The BIOS chooses the timing, typically the monitor's preferred one, and
 * VBE has no way to ask what it picked.  Time the CRTC's frame counter
 * against the performance counter instead.  Returns Hz, or 0.
 */
static ULONG MeasureRefresh(PGCN_EXT Ext, ULONG CrtcOffset)
{
    const ULONG frames = 12;
    ULONG reg = mmCRTC_STATUS_FRAME_COUNT + CrtcOffset;
    LARGE_INTEGER freq, t0, t1;
    ULONG c, spins, lo, hi, f, fh, per;

    c = RREG(Ext, reg);
    for (spins = 0; RREG(Ext, reg) == c; spins++) {
        if (spins > 20000)
            return 0;               /* counter not running */
        VideoPortStallExecution(10);
    }
    t0.QuadPart = VideoPortQueryPerformanceCounter(Ext, &freq.QuadPart);
    c = RREG(Ext, reg);
    for (spins = 0; ((RREG(Ext, reg) - c) & 0xFFFFFF) < frames; spins++) {
        if (spins > 60000)
            return 0;
        VideoPortStallExecution(10);
    }
    t1.QuadPart = VideoPortQueryPerformanceCounter(Ext, &freq.QuadPart);

    /* 64-bit arithmetic by halves: no compiler helpers in a miniport. */
    lo = t1.LowPart - t0.LowPart;
    hi = (ULONG)t1.HighPart - (ULONG)t0.HighPart -
         (t1.LowPart < t0.LowPart ? 1u : 0u);
    f = freq.LowPart;
    fh = (ULONG)freq.HighPart;
    while (hi != 0 || fh != 0) {
        lo = (lo >> 1) | (hi << 31);
        hi >>= 1;
        f = (f >> 1) | (fh << 31);
        fh >>= 1;
    }
    per = lo / frames;              /* ticks per frame */
    if (per == 0)
        return 0;
    return (f + per / 2) / per;
}

static ULONG ModeHz(PGCN_EXT Ext, ULONG Index)
{
    if (Ext->ModeHz[Index] != 0)
        return Ext->ModeHz[Index];
    return Ext->DefaultHz != 0 ? Ext->DefaultHz : 60;
}

static VP_STATUS NTAPI RefreshRegCallback(PVOID HwExt, PVOID Context,
                                          PWSTR ValueName, PVOID ValueData,
                                          ULONG ValueLength)
{
    PGCN_EXT Ext = (PGCN_EXT)HwExt;

    (void)Context;
    (void)ValueName;
    if (ValueLength >= sizeof(ULONG) && *(PULONG)ValueData >= 20 &&
        *(PULONG)ValueData <= 500)
        Ext->DefaultHz = *(PULONG)ValueData;
    return NO_ERROR;
}

static VOID FillModeInfo(const GCN_MODE *m, ULONG Index,
                         PVIDEO_MODE_INFORMATION vi)
{
    memset(vi, 0, sizeof(*vi));
    vi->Length = sizeof(*vi);
    vi->ModeIndex = Index;
    vi->VisScreenWidth = m->Width;
    vi->VisScreenHeight = m->Height;
    vi->ScreenStride = m->Pitch;
    vi->NumberOfPlanes = 1;
    vi->BitsPerPlane = m->Bpp;
    vi->Frequency = 60;
    vi->XMillimeter = 320;
    vi->YMillimeter = 240;
    vi->NumberRedBits = m->RedSize;
    vi->NumberGreenBits = m->GreenSize;
    vi->NumberBlueBits = m->BlueSize;
    vi->RedMask = MaskOf(m->RedSize, m->RedPos);
    vi->GreenMask = MaskOf(m->GreenSize, m->GreenPos);
    vi->BlueMask = MaskOf(m->BlueSize, m->BluePos);
    vi->AttributeFlags = VIDEO_MODE_COLOR | VIDEO_MODE_GRAPHICS;
    vi->VideoMemoryBitmapWidth = m->Width;
    vi->VideoMemoryBitmapHeight = m->Height;
}

static VP_STATUS SetMode(PGCN_EXT Ext, ULONG Index)
{
    INT10_BIOS_ARGUMENTS a;
    const GCN_MODE *m;

    if (!VbeEnumerate(Ext) || Index >= Ext->NumModes)
        return ERROR_INVALID_PARAMETER;
    m = &Ext->Modes[Index];

    CursorHide(Ext);
    memset(&a, 0, sizeof(a));
    a.Eax = 0x4F02;
    a.Ebx = m->VbeMode | 0x4000; /* linear framebuffer */
    if (!VbeCall(Ext, &a)) {
        KLog(Ext, "mode: set %x failed, ax=%x\n", (ULONG)m->VbeMode, a.Eax);
        LogSave(Ext);
        return ERROR_INVALID_FUNCTION;
    }
    Ext->CurrentMode = (LONG)Index;
    KLog(Ext, "mode: %ux%ux%u set (vbe %x)\n", (ULONG)m->Width,
         (ULONG)m->Height, (ULONG)m->Bpp, (ULONG)m->VbeMode);
    DceProbe(Ext, m);
    {
        ULONG i, hz = 0;

        for (i = 0; i < GCN_MAX_CRTC && hz == 0; i++) {
            if (Ext->CrtcMask & (1u << i))
                hz = MeasureRefresh(Ext, g_CrtcOffsets[i]);
        }
        KLog(Ext, "mode: refresh %u Hz\n", hz);
        if (hz >= 20 && hz <= 500) {
            Ext->ModeHz[Index] = (USHORT)hz;
            if (Ext->DefaultHz != hz) {
                /* remembered so the mode list is right from next boot on */
                Ext->DefaultHz = hz;
                VideoPortSetRegistryParameters(Ext, L"gcn.RefreshHz", &hz,
                                               sizeof(hz));
            }
        }
    }
    LogSave(Ext);
    return NO_ERROR;
}

/*
 * Mappings and shadow buffers belong to the display driver surface that
 * asked for them.  During a mode change the new surface is created before
 * the old one is destroyed, so several can exist at once and each is
 * released by address.
 */
static VOID UnmapFb(PGCN_EXT Ext, PVOID Va)
{
    if (Va == NULL)
        return;
    VideoPortUnmapMemory(Ext, Va, 0);
    if (Va == Ext->FbVa) {
        Ext->FbVa = NULL;
        Ext->FbMapLen = 0;
    }
}

static VP_STATUS MapFb(PGCN_EXT Ext, PVIDEO_MEMORY_INFORMATION Out)
{
    const GCN_MODE *m;
    PHYSICAL_ADDRESS phys;
    ULONG need, len, space, avail;
    PVOID va = NULL;
    VP_STATUS st;

    if (Ext->CurrentMode < 0)
        return ERROR_INVALID_FUNCTION;
    m = &Ext->Modes[Ext->CurrentMode];

    /* A power-of-two length keeps the write-combining range simple. */
    need = (ULONG)m->Pitch * m->Height;
    for (len = 0x100000; len < need; len <<= 1)
        ;
    avail = Ext->FbLen - (m->PhysBase - Ext->FbPhys.LowPart);
    if (len > avail)
        len = avail;

    phys.HighPart = 0;
    phys.LowPart = m->PhysBase;
    space = VIDEO_MEMORY_SPACE_P6CACHE;
    st = VideoPortMapMemory(Ext, phys, &len, &space, &va);
    Ext->WriteCombined = (st == NO_ERROR);
    if (st != NO_ERROR) {
        KLog(Ext, "map: write-combined mapping failed (%u), retrying\n",
             (ULONG)st);
        space = VIDEO_MEMORY_SPACE_MEMORY;
        va = NULL;
        st = VideoPortMapMemory(Ext, phys, &len, &space, &va);
        if (st != NO_ERROR)
            return st;
    }
    Ext->FbVa = va;
    Ext->FbMapLen = len;
    KLog(Ext, "map: fb %08x len %x at %08x\n", m->PhysBase, len, (ULONG)va);
    LogSave(Ext);

    Out->VideoRamBase = va;
    Out->VideoRamLength = len;
    Out->FrameBufferBase = va;
    Out->FrameBufferLength = need;
    return NO_ERROR;
}

static VOID ResetDevice(PGCN_EXT Ext)
{
    INT10_BIOS_ARGUMENTS a;

    CursorHide(Ext);
    if (Ext->CurrentMode < 0 || !Ext->Int10Ok)
        return;
    memset(&a, 0, sizeof(a));
    a.Eax = 0x0003; /* 80x25 text */
    Ext->Int10.Int10CallBios(Ext->Int10.Context, &a);
    Ext->CurrentMode = -1;
    Ext->CrtcMask = 0;
    KLog(Ext, "mode: reset to text\n");
}

static VOID FillInfo(PGCN_EXT Ext, GCN_INFO *Info)
{
    memset(Info, 0, sizeof(*Info));
    Info->Version = GCN_VERSION;
    Info->VendorId = Ext->VendorId;
    Info->DeviceId = Ext->DeviceId;
    Info->Revision = Ext->Revision;
    Info->FbPhysLow = Ext->FbPhys.LowPart;
    Info->FbPhysHigh = (ULONG)Ext->FbPhys.HighPart;
    Info->FbApertureLen = Ext->FbLen;
    Info->MmioPhysLow = Ext->MmioPhys.LowPart;
    Info->MmioLen = Ext->MmioLen;
    Info->VramMB = Ext->VramMB;
    Info->McFbBaseLow = Ext->McFbBaseLow;
    Info->McFbBaseHigh = Ext->McFbBaseHigh;
    Info->VbeVersion = Ext->VbeVersion;
    Info->NumModes = Ext->NumModes;
    Info->CurrentMode = Ext->CurrentMode;
    if (Ext->CurrentMode >= 0) {
        const GCN_MODE *m = &Ext->Modes[Ext->CurrentMode];
        Info->Width = m->Width;
        Info->Height = m->Height;
        Info->Bpp = m->Bpp;
        Info->Pitch = m->Pitch;
        Info->ModePhysLow = m->PhysBase;
    }
    Info->CrtcMask = Ext->CrtcMask;
    Info->CursorOk = Ext->CursorOk;
    Info->CursorOffset = Ext->CursorOffset;
    Info->WriteCombined = Ext->WriteCombined;
#ifdef GCN_DEBUG
    Info->DebugBuild = 1;
#endif
}

/* ---- miniport entry points ---------------------------------------------- */

static VP_STATUS NTAPI GcnFindAdapter(PVOID HwExt, PVOID HwContext,
                                      PWSTR ArgumentString,
                                      PVIDEO_PORT_CONFIG_INFO Cfg,
                                      PUCHAR Again)
{
    PGCN_EXT Ext = (PGCN_EXT)HwExt;
    VIDEO_ACCESS_RANGE ranges[16];
    UCHAR pci[64];
    ULONG slot = 0, i, bar0, bar5, vramBytes, aperture;
    LONG fb = -1, mmio = -1;
    VP_STATUS st;

    (void)HwContext;
    (void)ArgumentString;
    *Again = 0;
    Ext->CurrentMode = -1;

    memset(ranges, 0, sizeof(ranges));
    KLog(Ext, "gcnmp %x starting\n", (ULONG)GCN_VERSION);
    LogSave(Ext);
    st = VideoPortGetAccessRanges(Ext, 0, NULL, 16, ranges, NULL, NULL,
                                  &slot);
    if (st != NO_ERROR) {
        KLog(Ext, "resources unavailable (%u)\n", (ULONG)st);
        LogSave(Ext);
        return st;
    }

    memset(pci, 0, sizeof(pci));
    VideoPortGetBusData(Ext, VP_PCI_CONFIGURATION, 0, pci, 0, sizeof(pci));
    Ext->VendorId = RdW(pci, 0x00);
    Ext->DeviceId = RdW(pci, 0x02);
    Ext->Revision = pci[0x08];
    bar0 = RdD(pci, 0x10) & ~0xFu;
    bar5 = RdD(pci, 0x24) & ~0xFu;

    /* Framebuffer aperture is BAR0, registers are BAR5. */
    for (i = 0; i < 16; i++) {
        if (ranges[i].RangeLength == 0 || ranges[i].RangeInIoSpace)
            continue;
        if (ranges[i].RangeStart.HighPart != 0)
            continue;
        if (bar0 != 0 && ranges[i].RangeStart.LowPart == bar0)
            fb = (LONG)i;
        else if (bar5 != 0 && ranges[i].RangeStart.LowPart == bar5)
            mmio = (LONG)i;
    }
    if (fb < 0 || mmio < 0) {
        /* Fall back on size: largest range is VRAM, smallest is registers. */
        for (i = 0; i < 16; i++) {
            if (ranges[i].RangeLength == 0 || ranges[i].RangeInIoSpace ||
                ranges[i].RangeStart.HighPart != 0)
                continue;
            if (fb < 0 ||
                ranges[i].RangeLength > ranges[fb].RangeLength)
                fb = (LONG)i;
            if (mmio < 0 ||
                ranges[i].RangeLength < ranges[mmio].RangeLength)
                mmio = (LONG)i;
        }
    }
    for (i = 0; i < 16; i++) {
        if (ranges[i].RangeLength != 0)
            KLog(Ext, "range %u: %s %08x len %x\n", i,
                 ranges[i].RangeInIoSpace ? "io " : "mem",
                 ranges[i].RangeStart.LowPart, ranges[i].RangeLength);
    }
    if (fb < 0 || mmio < 0 || fb == mmio) {
        KLog(Ext, "BARs not found (bar0 %08x bar5 %08x)\n", bar0, bar5);
        LogSave(Ext);
        return ERROR_DEV_NOT_EXIST;
    }

    Ext->FbPhys = ranges[fb].RangeStart;
    Ext->FbLen = ranges[fb].RangeLength;
    Ext->MmioPhys = ranges[mmio].RangeStart;
    Ext->MmioLen = ranges[mmio].RangeLength;
    Ext->Mmio = (volatile ULONG *)VideoPortGetDeviceBase(
        Ext, Ext->MmioPhys, Ext->MmioLen, VIDEO_MEMORY_SPACE_MEMORY);
    if (Ext->Mmio == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;

    KLog(Ext, "device %04x:%04x rev %02x\n", (ULONG)Ext->VendorId,
         (ULONG)Ext->DeviceId, (ULONG)Ext->Revision);
    KLog(Ext, "fb aperture %08x len %x, mmio %08x len %x\n",
         Ext->FbPhys.LowPart, Ext->FbLen, Ext->MmioPhys.LowPart,
         Ext->MmioLen);

    Ext->VramMB = RREG(Ext, mmCONFIG_MEMSIZE);
    KLog(Ext, "vram %u MB\n", Ext->VramMB);

    /*
     * Park the cursor image in VRAM well away from any framebuffer the
     * BIOS will set up, but inside the CPU-visible aperture.
     */
    aperture = Ext->FbLen;
    if (Ext->VramMB != 0 && Ext->VramMB < 4096) {
        vramBytes = Ext->VramMB << 20;
        if (vramBytes < aperture)
            aperture = vramBytes;
    }
    if (aperture >= 0x08000000)
        Ext->CursorOffset = 0x04000000;
    else if (aperture >= 0x00200000)
        Ext->CursorOffset = aperture - 0x00100000;
    else
        Ext->CursorOffset = 0;
    if (Ext->CursorOffset != 0) {
        PHYSICAL_ADDRESS cur = Ext->FbPhys;
        cur.LowPart += Ext->CursorOffset;
        Ext->CursorVa = (PULONG)VideoPortGetDeviceBase(
            Ext, cur, GCN_CURSOR_HW_DIM * GCN_CURSOR_HW_DIM * 4,
            VIDEO_MEMORY_SPACE_MEMORY);
    }

    VideoPortGetRegistryParameters(Ext, L"gcn.RefreshHz", FALSE,
                                   RefreshRegCallback, NULL);

    Cfg->VdmPhysicalVideoMemoryAddress.QuadPart = 0;
    Cfg->VdmPhysicalVideoMemoryLength = 0;
    Cfg->HardwareStateSize = 0;
    Cfg->NumEmulatorAccessEntries = 0;
    Cfg->EmulatorAccessEntries = NULL;
    Cfg->EmulatorAccessEntriesContext = 0;

    {
        ULONG mem = Ext->VramMB >= 2048 ? 0x7FF00000 : (Ext->VramMB << 20);
        SetRegString(Ext, L"HardwareInformation.ChipType",
                     L"AMD GCN (Polaris)");
        SetRegString(Ext, L"HardwareInformation.DacType", L"Internal");
        SetRegString(Ext, L"HardwareInformation.AdapterString",
                     L"AMD Radeon (gcn4xp)");
        SetRegString(Ext, L"HardwareInformation.BiosString", L"VESA BIOS");
        VideoPortSetRegistryParameters(Ext, L"HardwareInformation.MemorySize",
                                       &mem, sizeof(mem));
    }
    LogSave(Ext);
    return NO_ERROR;
}

static BOOLEAN NTAPI GcnInitialize(PVOID HwExt)
{
    PGCN_EXT Ext = (PGCN_EXT)HwExt;

    /* May fail this early in boot; the first mode query retries. */
    Int10Acquire(Ext);
    return TRUE;
}

static BOOLEAN NTAPI GcnStartIO(PVOID HwExt, PVIDEO_REQUEST_PACKET Rp)
{
    PGCN_EXT Ext = (PGCN_EXT)HwExt;
    VP_STATUS st = NO_ERROR;
    ULONG info = 0;

    switch (Rp->IoControlCode) {
    case IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES: {
        PVIDEO_NUM_MODES nm = (PVIDEO_NUM_MODES)Rp->OutputBuffer;
        if (Rp->OutputBufferLength < sizeof(*nm)) {
            st = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        VbeEnumerate(Ext);
        nm->NumModes = Ext->NumModes;
        nm->ModeInformationLength = sizeof(VIDEO_MODE_INFORMATION);
        info = sizeof(*nm);
        break;
    }
    case IOCTL_VIDEO_QUERY_AVAIL_MODES: {
        PVIDEO_MODE_INFORMATION vi =
            (PVIDEO_MODE_INFORMATION)Rp->OutputBuffer;
        ULONG i;
        VbeEnumerate(Ext);
        if (Rp->OutputBufferLength < Ext->NumModes * sizeof(*vi)) {
            st = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        for (i = 0; i < Ext->NumModes; i++)
        {
            FillModeInfo(&Ext->Modes[i], i, &vi[i]);
            vi[i].Frequency = ModeHz(Ext, i);
        }
        info = Ext->NumModes * sizeof(*vi);
        break;
    }
    case IOCTL_VIDEO_QUERY_CURRENT_MODE:
        if (Rp->OutputBufferLength < sizeof(VIDEO_MODE_INFORMATION)) {
            st = ERROR_INSUFFICIENT_BUFFER;
        } else if (Ext->CurrentMode < 0) {
            st = ERROR_INVALID_FUNCTION;
        } else {
            FillModeInfo(&Ext->Modes[Ext->CurrentMode],
                         (ULONG)Ext->CurrentMode,
                         (PVIDEO_MODE_INFORMATION)Rp->OutputBuffer);
            ((PVIDEO_MODE_INFORMATION)Rp->OutputBuffer)->Frequency =
                ModeHz(Ext, (ULONG)Ext->CurrentMode);
            info = sizeof(VIDEO_MODE_INFORMATION);
        }
        break;
    case IOCTL_VIDEO_SET_CURRENT_MODE:
        if (Rp->InputBufferLength < sizeof(VIDEO_MODE)) {
            st = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        st = SetMode(Ext,
                     ((PVIDEO_MODE)Rp->InputBuffer)->RequestedMode &
                         0x3FFFFFFF);
        break;
    case IOCTL_VIDEO_RESET_DEVICE:
        ResetDevice(Ext);
        break;
    case IOCTL_VIDEO_MAP_VIDEO_MEMORY:
        if (Rp->OutputBufferLength < sizeof(VIDEO_MEMORY_INFORMATION)) {
            st = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        st = MapFb(Ext, (PVIDEO_MEMORY_INFORMATION)Rp->OutputBuffer);
        if (st == NO_ERROR)
            info = sizeof(VIDEO_MEMORY_INFORMATION);
        break;
    case IOCTL_VIDEO_UNMAP_VIDEO_MEMORY:
        if (Rp->InputBufferLength < sizeof(VIDEO_MEMORY)) {
            st = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        UnmapFb(Ext,
                ((PVIDEO_MEMORY)Rp->InputBuffer)->RequestedVirtualAddress);
        break;

    case IOCTL_GCN_GET_INFO:
        if (Rp->OutputBufferLength < sizeof(GCN_INFO)) {
            st = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        FillInfo(Ext, (GCN_INFO *)Rp->OutputBuffer);
        info = sizeof(GCN_INFO);
        break;
    case IOCTL_GCN_READ_LOG: {
        PCHAR out = (PCHAR)Rp->OutputBuffer;
        ULONG n = Ext->LogPos < GCN_LOG_SIZE ? Ext->LogPos : GCN_LOG_SIZE;
        ULONG start = Ext->LogPos - n, i;
        if (n > Rp->OutputBufferLength) {
            start += n - Rp->OutputBufferLength;
            n = Rp->OutputBufferLength;
        }
        for (i = 0; i < n; i++)
            out[i] = Ext->Log[(start + i) % GCN_LOG_SIZE];
        info = n;
        break;
    }
    case IOCTL_GCN_ALLOC_SHADOW: {
        ULONG bytes;
        PVOID shadow;
        if (Rp->InputBufferLength < sizeof(ULONG) ||
            Rp->OutputBufferLength < sizeof(PVOID)) {
            st = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        bytes = *(PULONG)Rp->InputBuffer;
        if (bytes == 0 || bytes > 0x10000000) {
            st = ERROR_INVALID_PARAMETER;
            break;
        }
        shadow = VideoPortAllocatePool(Ext, VpPagedPool, bytes, GCN_POOL_TAG);
        if (shadow == NULL) {
            KLog(Ext, "shadow: %u bytes unavailable\n", bytes);
            st = ERROR_NOT_ENOUGH_MEMORY;
            break;
        }
        *(PVOID *)Rp->OutputBuffer = shadow;
        info = sizeof(PVOID);
        break;
    }
    case IOCTL_GCN_FREE_SHADOW:
        if (Rp->InputBufferLength < sizeof(PVOID)) {
            st = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        if (*(PVOID *)Rp->InputBuffer != NULL)
            VideoPortFreePool(Ext, *(PVOID *)Rp->InputBuffer);
        break;
    case IOCTL_GCN_SET_POINTER_SHAPE: {
        const GCN_POINTER_SHAPE *sh =
            (const GCN_POINTER_SHAPE *)Rp->InputBuffer;
        ULONG x, y;
        if (Rp->InputBufferLength < sizeof(*sh)) {
            st = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        if (!Ext->CursorOk || sh->Width > GCN_CURSOR_DIM ||
            sh->Height > GCN_CURSOR_DIM) {
            st = ERROR_INVALID_FUNCTION;
            break;
        }
        CursorHide(Ext);
        for (y = 0; y < GCN_CURSOR_HW_DIM; y++) {
            for (x = 0; x < GCN_CURSOR_HW_DIM; x++) {
                ULONG v = 0;
                if (x < sh->Width && y < sh->Height)
                    v = sh->Pixels[y * GCN_CURSOR_DIM + x];
                Ext->CursorVa[y * GCN_CURSOR_HW_DIM + x] = v;
            }
        }
        /* Read the image back: catches a wrong VRAM offset assumption. */
        {
            ULONG bad = 0;
            for (y = 0; y < GCN_CURSOR_HW_DIM; y++) {
                for (x = 0; x < GCN_CURSOR_HW_DIM; x++) {
                    ULONG v = 0;
                    if (x < sh->Width && y < sh->Height)
                        v = sh->Pixels[y * GCN_CURSOR_DIM + x];
                    if (Ext->CursorVa[y * GCN_CURSOR_HW_DIM + x] != v)
                        bad++;
                }
            }
            if (bad != 0) {
                KLog(Ext, "cursor: %u pixels did not read back\n", bad);
                Ext->CursorOk = FALSE;
                st = ERROR_INVALID_FUNCTION;
            }
        }
        break;
    }
    case IOCTL_GCN_SET_POINTER_POS:
        if (Rp->InputBufferLength < sizeof(GCN_POINTER_POS)) {
            st = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        CursorSet(Ext, (const GCN_POINTER_POS *)Rp->InputBuffer);
        break;
    case IOCTL_GCN_REG_READ: {
        GCN_REG_IO io;
        PULONG out = (PULONG)Rp->OutputBuffer;
        ULONG i;
        if (Rp->InputBufferLength < sizeof(io)) {
            st = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        io = *(const GCN_REG_IO *)Rp->InputBuffer; /* buffer is shared */
        if (io.Count > Rp->OutputBufferLength / sizeof(ULONG) ||
            io.Index >= Ext->MmioLen / 4 ||
            io.Count > Ext->MmioLen / 4 - io.Index) {
            st = ERROR_INVALID_PARAMETER;
            break;
        }
        for (i = 0; i < io.Count; i++)
            out[i] = RREG(Ext, io.Index + i);
        info = io.Count * sizeof(ULONG);
        break;
    }
#ifdef GCN_DEBUG
    case IOCTL_GCN_REG_WRITE: {
        const GCN_REG_IO *io = (const GCN_REG_IO *)Rp->InputBuffer;
        if (Rp->InputBufferLength < sizeof(*io) ||
            io->Index >= Ext->MmioLen / 4) {
            st = ERROR_INVALID_PARAMETER;
            break;
        }
        WREG(Ext, io->Index, io->Value);
        break;
    }
#endif
    default:
        st = ERROR_INVALID_FUNCTION;
        break;
    }

#ifdef GCN_DEBUG
    /* Trace everything but the high-frequency pointer and query traffic. */
    if (Rp->IoControlCode != IOCTL_GCN_SET_POINTER_POS &&
        Rp->IoControlCode != IOCTL_GCN_SET_POINTER_SHAPE &&
        Rp->IoControlCode != IOCTL_GCN_READ_LOG &&
        Rp->IoControlCode != IOCTL_GCN_REG_READ &&
        Rp->IoControlCode != IOCTL_GCN_GET_INFO) {
        KLog(Ext, "ioctl %x: status %u, %u bytes\n",
             (Rp->IoControlCode >> 2) & 0xFFF, (ULONG)st, info);
        LogSave(Ext);
    }
#endif
    Rp->StatusBlock->Status = st;
    Rp->StatusBlock->Information = info;
    return TRUE;
}

/*
 * Called at bugcheck / hibernate time at high IRQL.  Only register pokes
 * are allowed here; returning FALSE lets the HAL restore text mode.
 */
static BOOLEAN NTAPI GcnResetHw(PVOID HwExt, ULONG Columns, ULONG Rows)
{
    (void)Columns;
    (void)Rows;
    CursorHide((PGCN_EXT)HwExt);
    return FALSE;
}

static VP_STATUS NTAPI GcnGetPowerState(PVOID HwExt, ULONG HwId,
                                        PVIDEO_POWER_MANAGEMENT Pm)
{
    (void)HwExt;
    (void)HwId;
    (void)Pm;
    return NO_ERROR;
}

static VP_STATUS NTAPI GcnSetPowerState(PVOID HwExt, ULONG HwId,
                                        PVIDEO_POWER_MANAGEMENT Pm)
{
    PGCN_EXT Ext = (PGCN_EXT)HwExt;
    INT10_BIOS_ARGUMENTS a;
    ULONG dpms;

    if (HwId == DISPLAY_ADAPTER_HW_ID)
        return NO_ERROR;

    /* Monitor child: VBE/PM display power state. */
    switch (Pm->PowerState) {
    case VideoPowerOn:      dpms = 0; break;
    case VideoPowerStandBy: dpms = 1; break;
    case VideoPowerSuspend: dpms = 2; break;
    case VideoPowerOff:     dpms = 4; break;
    default:                return NO_ERROR;
    }
    if (Ext->Int10Ok && Ext->CurrentMode >= 0) {
        memset(&a, 0, sizeof(a));
        a.Eax = 0x4F10;
        a.Ebx = 0x01 | (dpms << 8);
        Ext->Int10.Int10CallBios(Ext->Int10.Context, &a);
    }
    return NO_ERROR;
}

static ULONG NTAPI GcnGetChildDescriptor(PVOID HwExt,
                                         PVIDEO_CHILD_ENUM_INFO EnumInfo,
                                         PVIDEO_CHILD_TYPE Type,
                                         PUCHAR Descriptor, PULONG UId,
                                         PULONG Unused)
{
    (void)HwExt;
    (void)Descriptor;
    /* One generic monitor.  Index 0 is reserved for ACPI enumeration. */
    if (EnumInfo->ChildIndex == 1) {
        *Type = Monitor;
        *UId = 1;
        *Unused = 0;
        return VIDEO_ENUM_MORE_DEVICES;
    }
    return VIDEO_ENUM_NO_MORE_DEVICES;
}

#ifdef GCN_DEBUG
/*
 * Bring-up aid: trace PnP requests sent to this driver and the status the
 * video port's handler returns, into the registry value "DebugPnp" as
 * (minor function, start resource count, status) triples.  Uses fixed x86
 * layouts of DRIVER_OBJECT / IRP / IO_STACK_LOCATION.
 */
#define DBG_IRP_MJ_PNP        0x1B
#define DBG_DRVOBJ_MAJORFN    0x38  /* DRIVER_OBJECT.MajorFunction */
#define DBG_IRP_CURSTACK      0x60  /* IRP.Tail.Overlay.CurrentStackLocation */

typedef LONG (NTAPI *DBG_DISPATCH)(PVOID DeviceObject, PVOID Irp);

static DBG_DISPATCH g_DbgOrigPnp;
static ULONG g_DbgPnpTrace[3 * 24];
static ULONG g_DbgPnpCount;

static LONG NTAPI DbgPnpDispatch(PVOID DeviceObject, PVOID Irp)
{
    const UCHAR *sp = *(const UCHAR **)((PUCHAR)Irp + DBG_IRP_CURSTACK);
    ULONG minor = sp[1];
    ULONG resCount = 0xFFFFFFFF;
    LONG status;

    if (minor == 0) { /* IRP_MN_START_DEVICE: count allocated resources */
        const ULONG *list = *(const ULONG **)(sp + 4);
        /* CM_RESOURCE_LIST: Count, then full descriptor whose partial
           list count sits at offset 16 */
        resCount = list == NULL ? 0 : (list[0] == 0 ? 0 : list[4]);
    }
    status = g_DbgOrigPnp(DeviceObject, Irp);
    if (g_DbgPnpCount < 24) {
        ULONG *t = &g_DbgPnpTrace[3 * g_DbgPnpCount++];
        t[0] = minor;
        t[1] = resCount;
        t[2] = (ULONG)status;
        DbgReg(L"DebugPnp", REG_BINARY, g_DbgPnpTrace,
               g_DbgPnpCount * 3 * sizeof(ULONG));
    }
    return status;
}

static VOID DbgHookPnp(PVOID DriverObject)
{
    DBG_DISPATCH *table =
        (DBG_DISPATCH *)((PUCHAR)DriverObject + DBG_DRVOBJ_MAJORFN);

    g_DbgOrigPnp = table[DBG_IRP_MJ_PNP];
    table[DBG_IRP_MJ_PNP] = DbgPnpDispatch;
}
#endif

/*
 * Legacy VGA resources the card decodes besides its PCI BARs.  The video
 * port needs these declared for a VGA-class device.
 */
static VIDEO_ACCESS_RANGE g_LegacyResources[] = {
    { { 0x000003B0, 0 }, 0x0000000C, 1, 1, 1, 0 },
    { { 0x000003C0, 0 }, 0x00000020, 1, 1, 1, 0 },
    { { 0x000A0000, 0 }, 0x00020000, 0, 1, 1, 0 },
};

ULONG NTAPI DriverEntry(PVOID Context1, PVOID Context2)
{
    VIDEO_HW_INITIALIZATION_DATA init;

    memset(&init, 0, sizeof(init));
    init.HwInitDataSize = sizeof(init);
    init.AdapterInterfaceType = VP_PCI_BUS;
    init.HwFindAdapter = GcnFindAdapter;
    init.HwInitialize = GcnInitialize;
    init.HwStartIO = GcnStartIO;
    init.HwResetHw = GcnResetHw;
    init.HwSetPowerState = GcnSetPowerState;
    init.HwGetPowerState = GcnGetPowerState;
    init.HwGetVideoChildDescriptor = GcnGetChildDescriptor;
    init.HwDeviceExtensionSize = sizeof(GCN_EXT);
    init.HwLegacyResourceList = g_LegacyResources;
    init.HwLegacyResourceCount =
        sizeof(g_LegacyResources) / sizeof(g_LegacyResources[0]);
    {
        ULONG status = 0xFFFFFFFF;

        DbgReg(L"DebugEntry", REG_DWORD, &status, sizeof(status));
        status = VideoPortInitialize(Context1, Context2, &init, NULL);
        DbgReg(L"DebugEntry", REG_DWORD, &status, sizeof(status));
#ifdef GCN_DEBUG
        if (status == 0)
            DbgHookPnp(Context1);
#endif
        return status;
    }
}
