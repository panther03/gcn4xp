/*
 * screen.c - mode selection and PDEV / surface setup.
 */
#include "driver.h"

BOOL bMiniport(PPDEV ppdev, DWORD code, PVOID in, DWORD cjIn, PVOID out,
               DWORD cjOut, DWORD *pcjRet)
{
    DWORD cj = 0;

    if (EngDeviceIoControl(ppdev->hDriver, code, in, cjIn, out, cjOut, &cj))
        return FALSE;
    if (pcjRet != NULL)
        *pcjRet = cj;
    return TRUE;
}

/*
 * Returns the miniport's mode table (caller frees with EngFreeMem) with
 * unusable entries marked by Length == 0.
 */
DWORD cGetModes(HANDLE hDriver, PVIDEO_MODE_INFORMATION *ppModes)
{
    VIDEO_NUM_MODES nm;
    PVIDEO_MODE_INFORMATION pm;
    DWORD cj, i;

    *ppModes = NULL;
    if (EngDeviceIoControl(hDriver, IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES, NULL,
                           0, &nm, sizeof(nm), &cj) ||
        nm.NumModes == 0 ||
        nm.ModeInformationLength != sizeof(VIDEO_MODE_INFORMATION))
        return 0;

    pm = EngAllocMem(FL_ZERO_MEMORY, nm.NumModes * sizeof(*pm),
                     GCN_ALLOC_TAG);
    if (pm == NULL)
        return 0;
    if (EngDeviceIoControl(hDriver, IOCTL_VIDEO_QUERY_AVAIL_MODES, NULL, 0,
                           pm, nm.NumModes * sizeof(*pm), &cj)) {
        EngFreeMem(pm);
        return 0;
    }
    for (i = 0; i < nm.NumModes; i++) {
        if (pm[i].NumberOfPlanes != 1 ||
            !(pm[i].AttributeFlags & VIDEO_MODE_GRAPHICS) ||
            (pm[i].BitsPerPlane != 16 && pm[i].BitsPerPlane != 32))
            pm[i].Length = 0;
    }
    *ppModes = pm;
    return nm.NumModes;
}

static const LOGFONTW g_lfSystem = {
    16, 7, 0, 0, 700, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
    CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, VARIABLE_PITCH | FF_DONTCARE,
    L"System"
};
static const LOGFONTW g_lfAnsiVar = {
    12, 9, 0, 0, 400, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
    CLIP_STROKE_PRECIS, PROOF_QUALITY, VARIABLE_PITCH | FF_DONTCARE,
    L"MS Sans Serif"
};
static const LOGFONTW g_lfAnsiFix = {
    12, 9, 0, 0, 400, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
    CLIP_STROKE_PRECIS, PROOF_QUALITY, FIXED_PITCH | FF_DONTCARE,
    L"Courier"
};

/* Picks the mode described by the DEVMODE and fills GDIINFO / DEVINFO. */
BOOL bInitPDEV(PPDEV ppdev, DEVMODEW *pdm, GDIINFO *pgdi, DEVINFO *pdi)
{
    PVIDEO_MODE_INFORMATION pModes, pSel = NULL;
    DWORD cModes, i;
    BOOL bDefault;

    cModes = cGetModes(ppdev->hDriver, &pModes);
    if (cModes == 0)
        return FALSE;

    /*
     * The refresh rate is whatever the BIOS programs for a resolution and
     * is only known once the mode has been set, so it is reported but
     * never used to pick a mode.
     */
    bDefault = pdm->dmBitsPerPel == 0 && pdm->dmPelsWidth == 0 &&
               pdm->dmPelsHeight == 0 && pdm->dmDisplayFrequency == 0;
    for (i = 0; i < cModes && pSel == NULL; i++) {
        if (pModes[i].Length == 0)
            continue;
        if (bDefault ||
            (pModes[i].VisScreenWidth == pdm->dmPelsWidth &&
             pModes[i].VisScreenHeight == pdm->dmPelsHeight &&
             pModes[i].BitsPerPlane == pdm->dmBitsPerPel))
            pSel = &pModes[i];
    }
    if (pSel == NULL) {
        EngFreeMem(pModes);
        return FALSE;
    }

    ppdev->ulMode = pSel->ModeIndex;
    ppdev->cx = (LONG)pSel->VisScreenWidth;
    ppdev->cy = (LONG)pSel->VisScreenHeight;
    ppdev->cBitsPixel = pSel->BitsPerPlane;
    ppdev->cjPixel = pSel->BitsPerPlane / 8;
    ppdev->lDeltaScreen = (LONG)pSel->ScreenStride;
    ppdev->lDeltaShadow = ppdev->cx * (LONG)ppdev->cjPixel;
    ppdev->iBitmapFormat = ppdev->cBitsPixel == 32 ? BMF_32BPP : BMF_16BPP;
    ppdev->flRed = pSel->RedMask;
    ppdev->flGreen = pSel->GreenMask;
    ppdev->flBlue = pSel->BlueMask;

    memset(pgdi, 0, sizeof(*pgdi));
    pgdi->ulVersion = GDI_DRIVER_VERSION;
    pgdi->ulTechnology = DT_RASDISPLAY;
    pgdi->ulHorzSize = pSel->XMillimeter;
    pgdi->ulVertSize = pSel->YMillimeter;
    pgdi->ulHorzRes = (ULONG)ppdev->cx;
    pgdi->ulVertRes = (ULONG)ppdev->cy;
    pgdi->ulPanningHorzRes = (ULONG)ppdev->cx;
    pgdi->ulPanningVertRes = (ULONG)ppdev->cy;
    pgdi->cBitsPixel = ppdev->cBitsPixel;
    pgdi->cPlanes = 1;
    pgdi->ulNumColors = (ULONG)-1;
    pgdi->ulVRefresh = pSel->Frequency;
    pgdi->ulBltAlignment = 1;
    pgdi->ulLogPixelsX = pdm->dmLogPixels ? pdm->dmLogPixels : 96;
    pgdi->ulLogPixelsY = pgdi->ulLogPixelsX;
    pgdi->flTextCaps = TC_RA_ABLE;
    pgdi->ulDACRed = pSel->NumberRedBits;
    pgdi->ulDACGreen = pSel->NumberGreenBits;
    pgdi->ulDACBlue = pSel->NumberBlueBits;
    pgdi->ulAspectX = 0x24;
    pgdi->ulAspectY = 0x24;
    pgdi->ulAspectXY = 0x33;
    pgdi->xStyleStep = 1;
    pgdi->yStyleStep = 1;
    pgdi->denStyleStep = 3;
    pgdi->ulNumPalReg = 0;
    /* sRGB-like primaries, same defaults the stock framebuffer driver uses */
    pgdi->ciDevice.Red.x = 6700;
    pgdi->ciDevice.Red.y = 3300;
    pgdi->ciDevice.Green.x = 2100;
    pgdi->ciDevice.Green.y = 7100;
    pgdi->ciDevice.Blue.x = 1400;
    pgdi->ciDevice.Blue.y = 800;
    pgdi->ciDevice.AlignmentWhite.x = 3127;
    pgdi->ciDevice.AlignmentWhite.y = 3290;
    pgdi->ciDevice.AlignmentWhite.Y = 0;
    pgdi->ciDevice.RedGamma = 20000;
    pgdi->ciDevice.GreenGamma = 20000;
    pgdi->ciDevice.BlueGamma = 20000;
    pgdi->ulPrimaryOrder = PRIMARY_ORDER_CBA;
    pgdi->ulHTPatternSize = HT_PATSIZE_4x4_M;
    pgdi->ulHTOutputFormat =
        ppdev->cBitsPixel == 32 ? HT_FORMAT_32BPP : HT_FORMAT_16BPP;
    pgdi->flHTFlags = HT_FLAG_ADDITIVE_PRIMS;

    memset(pdi, 0, sizeof(*pdi));
    pdi->flGraphicsCaps = GCAPS_OPAQUERECT | GCAPS_ALTERNATEFILL |
                          GCAPS_WINDINGFILL;
    pdi->flGraphicsCaps2 = GCAPS2_ALPHACURSOR;
    pdi->lfDefaultFont = g_lfSystem;
    pdi->lfAnsiVarFont = g_lfAnsiVar;
    pdi->lfAnsiFixFont = g_lfAnsiFix;
    pdi->cFonts = 0;
    pdi->iDitherFormat = ppdev->iBitmapFormat;
    pdi->cxDither = 8;
    pdi->cyDither = 8;
    ppdev->hpalDefault = EngCreatePalette(PAL_BITFIELDS, 0, NULL,
                                          ppdev->flRed, ppdev->flGreen,
                                          ppdev->flBlue);
    pdi->hpalDefault = ppdev->hpalDefault;

    EngFreeMem(pModes);
    return ppdev->hpalDefault != NULL;
}
