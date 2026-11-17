/*
 * pointer.c - hardware cursor support.
 *
 * The display controller composites a 64x64 premultiplied-ARGB cursor.
 * Alpha, colour and plain black/white monochrome pointers are converted to
 * that format; pointers that need screen inversion (XOR pixels) or are too
 * large are declined so GDI draws them in software through our hooks.
 */
#include "driver.h"

static VOID vSendPos(PPDEV ppdev)
{
    bMiniport(ppdev, IOCTL_GCN_SET_POINTER_POS, &ppdev->ptr,
              sizeof(ppdev->ptr), NULL, 0, NULL);
}

VOID vPointerDisable(PPDEV ppdev)
{
    GCN_POINTER_POS pos = ppdev->ptr;

    if (!ppdev->bHwPointer)
        return;
    pos.Visible = 0;
    bMiniport(ppdev, IOCTL_GCN_SET_POINTER_POS, &pos, sizeof(pos), NULL, 0,
              NULL);
}

/* Re-establishes the hardware cursor after a mode set. */
VOID vPointerRestore(PPDEV ppdev)
{
    if (!ppdev->bHwPointer)
        return;
    if (!ppdev->bHwPointerCapable ||
        !bMiniport(ppdev, IOCTL_GCN_SET_POINTER_SHAPE, &ppdev->shape,
                   sizeof(ppdev->shape), NULL, 0, NULL)) {
        /* GDI will hand us the shape again and we will decline it. */
        ppdev->bHwPointer = FALSE;
        return;
    }
    vSendPos(ppdev);
}

static __inline BOOL bMonoBit(const BYTE *row, LONG x)
{
    return (row[x >> 3] & (0x80 >> (x & 7))) != 0;
}

static ULONG ulGetPixel(SURFOBJ *pso, LONG x, LONG y)
{
    const BYTE *row = (const BYTE *)pso->pvScan0 + y * pso->lDelta;

    switch (pso->iBitmapFormat) {
    case BMF_1BPP:
        return bMonoBit(row, x) ? 1 : 0;
    case BMF_4BPP:
        return (x & 1) ? (row[x >> 1] & 0x0F) : (row[x >> 1] >> 4);
    case BMF_8BPP:
        return row[x];
    case BMF_16BPP:
        return *(const USHORT *)(row + x * 2);
    case BMF_24BPP:
        return row[x * 3] | (row[x * 3 + 1] << 8) | (row[x * 3 + 2] << 16);
    case BMF_32BPP:
        return *(const ULONG *)(row + x * 4);
    }
    return 0;
}

/* Expands one colour channel of a device pixel to 8 bits. */
static ULONG ulChannel(ULONG pixel, FLONG mask)
{
    ULONG v, bits = 0, m;

    if (mask == 0)
        return 0;
    v = pixel & mask;
    while (!(mask & 1)) {
        mask >>= 1;
        v >>= 1;
    }
    for (m = mask; m != 0; m >>= 1)
        bits++;
    if (bits >= 8)
        return v >> (bits - 8);
    return (v << (8 - bits)) | (v >> (2 * bits > 8 ? 2 * bits - 8 : 0));
}

static ULONG ulDeviceToArgb(PPDEV ppdev, ULONG pixel)
{
    return 0xFF000000 | (ulChannel(pixel, ppdev->flRed) << 16) |
           (ulChannel(pixel, ppdev->flGreen) << 8) |
           ulChannel(pixel, ppdev->flBlue);
}

/*
 * The hardware blends premultiplied alpha: out = src + (1 - a) * dst.  If
 * any pixel has a colour channel brighter than its alpha, the image cannot
 * be premultiplied already (stray colour in transparent pixels would show
 * up as speckle), so scale every pixel's colour by its alpha.
 */
static VOID vPremultiply(GCN_POINTER_SHAPE *psh)
{
    ULONG i, n = GCN_CURSOR_DIM * GCN_CURSOR_DIM;
    BOOL bStraight = FALSE;

    for (i = 0; i < n && !bStraight; i++) {
        ULONG p = psh->Pixels[i], a = p >> 24;

        if (((p >> 16) & 0xFF) > a || ((p >> 8) & 0xFF) > a ||
            (p & 0xFF) > a)
            bStraight = TRUE;
    }
    if (!bStraight)
        return;
    for (i = 0; i < n; i++) {
        ULONG p = psh->Pixels[i], a = p >> 24;
        ULONG r = (((p >> 16) & 0xFF) * a + 127) / 255;
        ULONG g = (((p >> 8) & 0xFF) * a + 127) / 255;
        ULONG b = ((p & 0xFF) * a + 127) / 255;

        psh->Pixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
}

/* Converts a GDI pointer to premultiplied ARGB; FALSE if not expressible. */
static BOOL bConvertShape(PPDEV ppdev, SURFOBJ *psoMask, SURFOBJ *psoColor,
                          XLATEOBJ *pxlo, FLONG fl, GCN_POINTER_SHAPE *psh)
{
    LONG cx, cy, x, y;
    ULONG *dst;

    if (psoColor != NULL) {
        cx = psoColor->sizlBitmap.cx;
        cy = psoColor->sizlBitmap.cy;
    } else {
        cx = psoMask->sizlBitmap.cx;
        cy = psoMask->sizlBitmap.cy / 2;
    }
    if (cx <= 0 || cy <= 0 || cx > GCN_CURSOR_DIM || cy > GCN_CURSOR_DIM)
        return FALSE;
    if (psoMask != NULL &&
        (psoMask->iBitmapFormat != BMF_1BPP || psoMask->pvScan0 == NULL ||
         psoMask->sizlBitmap.cx < cx ||
         psoMask->sizlBitmap.cy < (psoColor != NULL ? cy : 2 * cy)))
        return FALSE;
    if (psoColor != NULL && psoColor->pvScan0 == NULL)
        return FALSE;

    memset(psh, 0, sizeof(*psh));
    psh->Width = (ULONG)cx;
    psh->Height = (ULONG)cy;

    for (y = 0; y < cy; y++) {
        dst = &psh->Pixels[y * GCN_CURSOR_DIM];

        if (psoColor != NULL && (fl & SPS_ALPHA)) {
            /* 32bpp BGRA, already premultiplied by the window manager */
            if (psoColor->iBitmapFormat != BMF_32BPP)
                return FALSE;
            for (x = 0; x < cx; x++)
                dst[x] = ulGetPixel(psoColor, x, y);
        } else if (psoColor != NULL) {
            const BYTE *and = psoMask == NULL ? NULL :
                (const BYTE *)psoMask->pvScan0 + y * psoMask->lDelta;

            for (x = 0; x < cx; x++) {
                ULONG pixel = ulGetPixel(psoColor, x, y);

                if (pxlo != NULL && !(pxlo->flXlate & XO_TRIVIAL))
                    pixel = XLATEOBJ_iXlate(pxlo, pixel);
                pixel = ulDeviceToArgb(ppdev, pixel);
                if (and != NULL && bMonoBit(and, x)) {
                    /* AND=1: screen shows through, XORed with the colour */
                    if (pixel & 0x00FFFFFF)
                        return FALSE;
                    dst[x] = 0;
                } else {
                    dst[x] = pixel;
                }
            }
        } else {
            const BYTE *and =
                (const BYTE *)psoMask->pvScan0 + y * psoMask->lDelta;
            const BYTE *xor =
                (const BYTE *)psoMask->pvScan0 + (y + cy) * psoMask->lDelta;

            for (x = 0; x < cx; x++) {
                BOOL a = bMonoBit(and, x), xr = bMonoBit(xor, x);

                if (a && xr)
                    return FALSE;           /* inverts the screen */
                dst[x] = a ? 0 : (xr ? 0xFFFFFFFF : 0xFF000000);
            }
        }
    }
    if (psoColor != NULL && (fl & SPS_ALPHA))
        vPremultiply(psh);
    return TRUE;
}

ULONG APIENTRY DrvSetPointerShape(SURFOBJ *pso, SURFOBJ *psoMask,
                                  SURFOBJ *psoColor, XLATEOBJ *pxlo,
                                  LONG xHot, LONG yHot, LONG x, LONG y,
                                  RECTL *prcl, FLONG fl)
{
    PPDEV ppdev = (PPDEV)pso->dhpdev;

    (void)prcl;

    if (psoMask == NULL && psoColor == NULL) {
        /* no pointer at all */
        vPointerDisable(ppdev);
        ppdev->bHwPointer = FALSE;
        return SPS_ACCEPT_NOEXCLUDE;
    }

    if (!ppdev->bEnabled || !ppdev->bHwPointerCapable || xHot < 0 ||
        yHot < 0 || xHot >= GCN_CURSOR_DIM || yHot >= GCN_CURSOR_DIM ||
        !bConvertShape(ppdev, psoMask, psoColor, pxlo, fl, &ppdev->shape) ||
        !bMiniport(ppdev, IOCTL_GCN_SET_POINTER_SHAPE, &ppdev->shape,
                   sizeof(ppdev->shape), NULL, 0, NULL)) {
        vPointerDisable(ppdev);
        ppdev->bHwPointer = FALSE;
        ppdev->stats.SwPointerSets++;
        return SPS_DECLINE;
    }

    ppdev->bHwPointer = TRUE;
    ppdev->stats.HwPointerSets++;
    ppdev->ptr.X = x;
    ppdev->ptr.Y = y;
    ppdev->ptr.HotX = (ULONG)xHot;
    ppdev->ptr.HotY = (ULONG)yHot;
    ppdev->ptr.Visible = (x != -1);
    vSendPos(ppdev);
    return SPS_ACCEPT_NOEXCLUDE;
}

VOID APIENTRY DrvMovePointer(SURFOBJ *pso, LONG x, LONG y, RECTL *prcl)
{
    PPDEV ppdev = (PPDEV)pso->dhpdev;

    (void)prcl;
    if (!ppdev->bHwPointer || !ppdev->bEnabled)
        return;
    ppdev->stats.PointerMoves++;
    if (x == -1) {
        ppdev->ptr.Visible = 0;
    } else {
        ppdev->ptr.X = x;
        ppdev->ptr.Y = y;
        ppdev->ptr.Visible = 1;
    }
    vSendPos(ppdev);
}
