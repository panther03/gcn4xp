/*
 * enable.c - driver entry points: PDEV and surface lifetime, mode list,
 * mode switching and the diagnostics escape.
 */
#include "driver.h"

BOOL APIENTRY DrvBitBlt(SURFOBJ *, SURFOBJ *, SURFOBJ *, CLIPOBJ *,
                        XLATEOBJ *, RECTL *, POINTL *, POINTL *, BRUSHOBJ *,
                        POINTL *, ROP4);
BOOL APIENTRY DrvCopyBits(SURFOBJ *, SURFOBJ *, CLIPOBJ *, XLATEOBJ *,
                          RECTL *, POINTL *);
BOOL APIENTRY DrvTextOut(SURFOBJ *, STROBJ *, FONTOBJ *, CLIPOBJ *, RECTL *,
                         RECTL *, BRUSHOBJ *, BRUSHOBJ *, POINTL *, MIX);
BOOL APIENTRY DrvStrokePath(SURFOBJ *, PATHOBJ *, CLIPOBJ *, XFORMOBJ *,
                            BRUSHOBJ *, POINTL *, LINEATTRS *, MIX);
BOOL APIENTRY DrvFillPath(SURFOBJ *, PATHOBJ *, CLIPOBJ *, BRUSHOBJ *,
                          POINTL *, MIX, FLONG);
BOOL APIENTRY DrvStrokeAndFillPath(SURFOBJ *, PATHOBJ *, CLIPOBJ *,
                                   XFORMOBJ *, BRUSHOBJ *, LINEATTRS *,
                                   BRUSHOBJ *, POINTL *, MIX, FLONG);
BOOL APIENTRY DrvLineTo(SURFOBJ *, CLIPOBJ *, BRUSHOBJ *, LONG, LONG, LONG,
                        LONG, RECTL *, MIX);
BOOL APIENTRY DrvStretchBlt(SURFOBJ *, SURFOBJ *, SURFOBJ *, CLIPOBJ *,
                            XLATEOBJ *, COLORADJUSTMENT *, POINTL *, RECTL *,
                            RECTL *, POINTL *, ULONG);
BOOL APIENTRY DrvStretchBltROP(SURFOBJ *, SURFOBJ *, SURFOBJ *, CLIPOBJ *,
                               XLATEOBJ *, COLORADJUSTMENT *, POINTL *,
                               RECTL *, RECTL *, POINTL *, ULONG, BRUSHOBJ *,
                               DWORD);
BOOL APIENTRY DrvAlphaBlend(SURFOBJ *, SURFOBJ *, CLIPOBJ *, XLATEOBJ *,
                            RECTL *, RECTL *, BLENDOBJ *);
BOOL APIENTRY DrvTransparentBlt(SURFOBJ *, SURFOBJ *, CLIPOBJ *, XLATEOBJ *,
                                RECTL *, RECTL *, ULONG, ULONG);
BOOL APIENTRY DrvGradientFill(SURFOBJ *, CLIPOBJ *, XLATEOBJ *, TRIVERTEX *,
                              ULONG, PVOID, ULONG, RECTL *, POINTL *, ULONG);
BOOL APIENTRY DrvPlgBlt(SURFOBJ *, SURFOBJ *, SURFOBJ *, CLIPOBJ *,
                        XLATEOBJ *, COLORADJUSTMENT *, POINTL *, POINTFIX *,
                        RECTL *, POINTL *, ULONG);
ULONG APIENTRY DrvSetPointerShape(SURFOBJ *, SURFOBJ *, SURFOBJ *, XLATEOBJ *,
                                  LONG, LONG, LONG, LONG, RECTL *, FLONG);
VOID APIENTRY DrvMovePointer(SURFOBJ *, LONG, LONG, RECTL *);

#define GCN_HOOKS                                                          \
    (HOOK_BITBLT | HOOK_COPYBITS | HOOK_TEXTOUT | HOOK_STROKEPATH |        \
     HOOK_FILLPATH | HOOK_STROKEANDFILLPATH | HOOK_LINETO |                \
     HOOK_STRETCHBLT | HOOK_STRETCHBLTROP | HOOK_ALPHABLEND |              \
     HOOK_TRANSPARENTBLT | HOOK_GRADIENTFILL | HOOK_PLGBLT)

DHPDEV APIENTRY DrvEnablePDEV(DEVMODEW *pdm, LPWSTR pwszLogAddress,
                              ULONG cPat, HSURF *phsurfPatterns, ULONG cjCaps,
                              ULONG *pdevcaps, ULONG cjDevInfo, DEVINFO *pdi,
                              HDEV hdev, LPWSTR pwszDeviceName,
                              HANDLE hDriver)
{
    GDIINFO gdi;
    DEVINFO di;
    PPDEV ppdev;

    (void)pwszLogAddress;
    (void)cPat;
    (void)phsurfPatterns;
    (void)hdev;
    (void)pwszDeviceName;

    ppdev = EngAllocMem(FL_ZERO_MEMORY, sizeof(PDEV), GCN_ALLOC_TAG);
    if (ppdev == NULL)
        return NULL;
    ppdev->hDriver = hDriver;

    if (!bInitPDEV(ppdev, pdm, &gdi, &di)) {
        EngFreeMem(ppdev);
        return NULL;
    }
    memcpy(pdevcaps, &gdi, min(cjCaps, sizeof(gdi)));
    memcpy(pdi, &di, min(cjDevInfo, sizeof(di)));
    return (DHPDEV)ppdev;
}

VOID APIENTRY DrvCompletePDEV(DHPDEV dhpdev, HDEV hdev)
{
    ((PPDEV)dhpdev)->hdevEng = hdev;
}

VOID APIENTRY DrvDisablePDEV(DHPDEV dhpdev)
{
    PPDEV ppdev = (PPDEV)dhpdev;

    if (ppdev->hpalDefault != NULL)
        EngDeletePalette(ppdev->hpalDefault);
    EngFreeMem(ppdev);
}

static BOOL bSetMode(PPDEV ppdev)
{
    VIDEO_MODE vm;

    vm.RequestedMode = ppdev->ulMode;
    return bMiniport(ppdev, IOCTL_VIDEO_SET_CURRENT_MODE, &vm, sizeof(vm),
                     NULL, 0, NULL);
}

static VOID vQueryPointerCaps(PPDEV ppdev)
{
    GCN_INFO info;

    ppdev->bHwPointerCapable =
        bMiniport(ppdev, IOCTL_GCN_GET_INFO, NULL, 0, &info, sizeof(info),
                  NULL) &&
        info.CursorOk;
}

VOID APIENTRY DrvDisableSurface(DHPDEV dhpdev)
{
    PPDEV ppdev = (PPDEV)dhpdev;
    VIDEO_MEMORY vmem;

    ppdev->bEnabled = FALSE;
    if (ppdev->hsurfDev != NULL) {
        EngDeleteSurface(ppdev->hsurfDev);
        ppdev->hsurfDev = NULL;
    }
    if (ppdev->psoShadow != NULL) {
        EngUnlockSurface(ppdev->psoShadow);
        ppdev->psoShadow = NULL;
    }
    if (ppdev->hsurfShadow != NULL) {
        EngDeleteSurface(ppdev->hsurfShadow);
        ppdev->hsurfShadow = NULL;
    }
    if (ppdev->pjShadow != NULL) {
        PVOID pv = ppdev->pjShadow;

        bMiniport(ppdev, IOCTL_GCN_FREE_SHADOW, &pv, sizeof(pv), NULL, 0,
                  NULL);
        ppdev->pjShadow = NULL;
    }
    if (ppdev->pjScreen != NULL) {
        vmem.RequestedVirtualAddress = ppdev->pjScreen;
        bMiniport(ppdev, IOCTL_VIDEO_UNMAP_VIDEO_MEMORY, &vmem, sizeof(vmem),
                  NULL, 0, NULL);
        ppdev->pjScreen = NULL;
    }
}

HSURF APIENTRY DrvEnableSurface(DHPDEV dhpdev)
{
    PPDEV ppdev = (PPDEV)dhpdev;
    VIDEO_MEMORY vmem;
    VIDEO_MEMORY_INFORMATION vmi;
    ULONG cjShadow;
    PVOID pvShadow = NULL;
    SIZEL sizl;

    if (!bSetMode(ppdev))
        return NULL;

    vmem.RequestedVirtualAddress = NULL;
    if (!bMiniport(ppdev, IOCTL_VIDEO_MAP_VIDEO_MEMORY, &vmem, sizeof(vmem),
                   &vmi, sizeof(vmi), NULL))
        goto fail;
    ppdev->pjScreen = (PBYTE)vmi.FrameBufferBase;

    cjShadow = (ULONG)ppdev->lDeltaShadow * (ULONG)ppdev->cy;
    if (!bMiniport(ppdev, IOCTL_GCN_ALLOC_SHADOW, &cjShadow, sizeof(cjShadow),
                   &pvShadow, sizeof(pvShadow), NULL) ||
        pvShadow == NULL)
        goto fail;
    ppdev->pjShadow = (PBYTE)pvShadow;
    memset(ppdev->pjShadow, 0, cjShadow);

    sizl.cx = ppdev->cx;
    sizl.cy = ppdev->cy;
    ppdev->hsurfShadow =
        (HSURF)EngCreateBitmap(sizl, ppdev->lDeltaShadow,
                               ppdev->iBitmapFormat, BMF_TOPDOWN,
                               ppdev->pjShadow);
    if (ppdev->hsurfShadow == NULL ||
        !EngAssociateSurface(ppdev->hsurfShadow, ppdev->hdevEng, 0))
        goto fail;
    ppdev->psoShadow = EngLockSurface(ppdev->hsurfShadow);
    if (ppdev->psoShadow == NULL)
        goto fail;

    ppdev->hsurfDev =
        EngCreateDeviceSurface((DHSURF)ppdev, sizl, ppdev->iBitmapFormat);
    if (ppdev->hsurfDev == NULL ||
        !EngAssociateSurface(ppdev->hsurfDev, ppdev->hdevEng, GCN_HOOKS))
        goto fail;

    vQueryPointerCaps(ppdev);
    ppdev->bHwPointer = FALSE;
    ppdev->bEnabled = TRUE;
    vFlushAll(ppdev);
    return ppdev->hsurfDev;

fail:
    DrvDisableSurface(dhpdev);
    bMiniport(ppdev, IOCTL_VIDEO_RESET_DEVICE, NULL, 0, NULL, 0, NULL);
    return NULL;
}

BOOL APIENTRY DrvAssertMode(DHPDEV dhpdev, BOOL bEnable)
{
    PPDEV ppdev = (PPDEV)dhpdev;

    if (!bEnable) {
        vPointerDisable(ppdev);
        ppdev->bEnabled = FALSE;
        return bMiniport(ppdev, IOCTL_VIDEO_RESET_DEVICE, NULL, 0, NULL, 0,
                         NULL);
    }
    if (!bSetMode(ppdev))
        return FALSE;
    vQueryPointerCaps(ppdev);
    ppdev->bEnabled = TRUE;
    vFlushAll(ppdev);
    vPointerRestore(ppdev);
    return TRUE;
}

ULONG APIENTRY DrvGetModes(HANDLE hDriver, ULONG cjSize, DEVMODEW *pdm)
{
    PVIDEO_MODE_INFORMATION pModes;
    DWORD cModes, i;
    ULONG cjOut = 0, cValid = 0;

    cModes = cGetModes(hDriver, &pModes);
    if (cModes == 0)
        return 0;

    for (i = 0; i < cModes; i++) {
        if (pModes[i].Length == 0)
            continue;
        cValid++;
        if (pdm == NULL || cjSize - cjOut < sizeof(DEVMODEW))
            continue;
        memset(pdm, 0, sizeof(DEVMODEW));
        memcpy(pdm->dmDeviceName, GCN_DLL_NAME, sizeof(GCN_DLL_NAME));
        pdm->dmSpecVersion = DM_SPECVERSION;
        pdm->dmDriverVersion = DM_SPECVERSION;
        pdm->dmSize = sizeof(DEVMODEW);
        pdm->dmDriverExtra = 0;
        pdm->dmBitsPerPel = pModes[i].BitsPerPlane;
        pdm->dmPelsWidth = pModes[i].VisScreenWidth;
        pdm->dmPelsHeight = pModes[i].VisScreenHeight;
        pdm->dmDisplayFrequency = pModes[i].Frequency;
        pdm->dmDisplayFlags = 0;
        pdm->dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT |
                        DM_DISPLAYFREQUENCY | DM_DISPLAYFLAGS;
        pdm = (DEVMODEW *)((PBYTE)pdm + sizeof(DEVMODEW));
        cjOut += sizeof(DEVMODEW);
    }
    EngFreeMem(pModes);

    /* With no buffer, report the size needed for the whole list. */
    return pdm == NULL ? cValid * sizeof(DEVMODEW) : cjOut;
}

/* Compares the shadow against what is really in video memory. */
static VOID vVerify(PPDEV ppdev, GCN_VERIFY *pv)
{
    LONG x, y;
    ULONG cjRow = (ULONG)ppdev->cx * ppdev->cjPixel;

    memset(pv, 0, sizeof(*pv));
    if (!ppdev->bEnabled)
        return;
    for (y = 0; y < ppdev->cy; y++) {
        const BYTE *s = ppdev->pjShadow + y * ppdev->lDeltaShadow;
        const BYTE *d = ppdev->pjScreen + y * ppdev->lDeltaScreen;

        for (x = 0; x < (LONG)cjRow; x++) {
            if (s[x] != d[x]) {
                if (pv->BytesDifferent == 0) {
                    pv->FirstDiffX = (ULONG)x / ppdev->cjPixel;
                    pv->FirstDiffY = (ULONG)y;
                }
                pv->BytesDifferent++;
            }
        }
        pv->BytesCompared += cjRow;
    }
}

ULONG APIENTRY DrvEscape(SURFOBJ *pso, ULONG iEsc, ULONG cjIn, PVOID pvIn,
                         ULONG cjOut, PVOID pvOut)
{
    PPDEV ppdev = (PPDEV)pso->dhpdev;
    GCN_ESC_HEADER hdr;
    GCN_REG_IO io;
    DWORD cj = 0;

    if (iEsc == QUERYESCSUPPORT)
        return cjIn >= sizeof(ULONG) && *(ULONG *)pvIn == GCN_ESCAPE;
    if (iEsc != GCN_ESCAPE || cjIn < sizeof(hdr) || pvIn == NULL)
        return 0;
    hdr = *(GCN_ESC_HEADER *)pvIn;

    switch (hdr.Op) {
    case GCN_ESC_INFO:
        if (cjOut < sizeof(GCN_INFO) ||
            !bMiniport(ppdev, IOCTL_GCN_GET_INFO, NULL, 0, pvOut,
                       sizeof(GCN_INFO), &cj))
            return 0;
        return cj;
    case GCN_ESC_LOG:
        if (cjOut == 0 ||
            !bMiniport(ppdev, IOCTL_GCN_READ_LOG, NULL, 0, pvOut, cjOut, &cj))
            return 0;
        return cj;
    case GCN_ESC_REG_READ:
        io.Index = hdr.Arg0;
        io.Count = hdr.Arg1;
        io.Value = 0;
        if (!bMiniport(ppdev, IOCTL_GCN_REG_READ, &io, sizeof(io), pvOut,
                       cjOut, &cj))
            return 0;
        return cj;
#ifdef GCN_DEBUG
    case GCN_ESC_REG_WRITE:
        io.Index = hdr.Arg0;
        io.Count = 1;
        io.Value = hdr.Arg1;
        return bMiniport(ppdev, IOCTL_GCN_REG_WRITE, &io, sizeof(io), NULL, 0,
                         NULL);
    case GCN_ESC_MAP_VRAM: {
        GCN_VRAM_MAP m;
        m.Offset = hdr.Arg0;
        m.Length = hdr.Arg1;
        if (cjOut < sizeof(PVOID) ||
            !bMiniport(ppdev, IOCTL_GCN_MAP_VRAM, &m, sizeof(m), pvOut,
                       sizeof(PVOID), &cj))
            return 0;
        return cj;
    }
    case GCN_ESC_UNMAP_VRAM: {
        PVOID va = (PVOID)(ULONG_PTR)hdr.Arg0;
        return bMiniport(ppdev, IOCTL_GCN_UNMAP_VRAM, &va, sizeof(va), NULL,
                         0, NULL);
    }
#endif
    case GCN_ESC_STATS:
        if (cjOut < sizeof(GCN_STATS))
            return 0;
        memcpy(pvOut, &ppdev->stats, sizeof(GCN_STATS));
        return sizeof(GCN_STATS);
    case GCN_ESC_VERIFY:
        if (cjOut < sizeof(GCN_VERIFY))
            return 0;
        vVerify(ppdev, (GCN_VERIFY *)pvOut);
        return sizeof(GCN_VERIFY);
    }
    return 0;
}

VOID APIENTRY DrvDisableDriver(VOID)
{
}

static const DRVFN g_adrvfn[] = {
    { INDEX_DrvEnablePDEV,         (PFN)DrvEnablePDEV },
    { INDEX_DrvCompletePDEV,       (PFN)DrvCompletePDEV },
    { INDEX_DrvDisablePDEV,        (PFN)DrvDisablePDEV },
    { INDEX_DrvEnableSurface,      (PFN)DrvEnableSurface },
    { INDEX_DrvDisableSurface,     (PFN)DrvDisableSurface },
    { INDEX_DrvAssertMode,         (PFN)DrvAssertMode },
    { INDEX_DrvGetModes,           (PFN)DrvGetModes },
    { INDEX_DrvEscape,             (PFN)DrvEscape },
    { INDEX_DrvDisableDriver,      (PFN)DrvDisableDriver },
    { INDEX_DrvSetPointerShape,    (PFN)DrvSetPointerShape },
    { INDEX_DrvMovePointer,        (PFN)DrvMovePointer },
    { INDEX_DrvBitBlt,             (PFN)DrvBitBlt },
    { INDEX_DrvCopyBits,           (PFN)DrvCopyBits },
    { INDEX_DrvTextOut,            (PFN)DrvTextOut },
    { INDEX_DrvStrokePath,         (PFN)DrvStrokePath },
    { INDEX_DrvFillPath,           (PFN)DrvFillPath },
    { INDEX_DrvStrokeAndFillPath,  (PFN)DrvStrokeAndFillPath },
    { INDEX_DrvLineTo,             (PFN)DrvLineTo },
    { INDEX_DrvStretchBlt,         (PFN)DrvStretchBlt },
    { INDEX_DrvStretchBltROP,      (PFN)DrvStretchBltROP },
    { INDEX_DrvAlphaBlend,         (PFN)DrvAlphaBlend },
    { INDEX_DrvTransparentBlt,     (PFN)DrvTransparentBlt },
    { INDEX_DrvGradientFill,       (PFN)DrvGradientFill },
    { INDEX_DrvPlgBlt,             (PFN)DrvPlgBlt },
};

BOOL APIENTRY DrvEnableDriver(ULONG iEngineVersion, ULONG cj,
                              DRVENABLEDATA *pded)
{
    if (cj < sizeof(DRVENABLEDATA))
        return FALSE;
    pded->pdrvfn = (DRVFN *)g_adrvfn;
    pded->c = sizeof(g_adrvfn) / sizeof(g_adrvfn[0]);
    pded->iDriverVersion = iEngineVersion >= DDI_DRIVER_VERSION_NT5_01
                               ? DDI_DRIVER_VERSION_NT5_01
                               : DDI_DRIVER_VERSION_NT5;
    return TRUE;
}
