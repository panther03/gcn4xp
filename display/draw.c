/*
 * draw.c - drawing hooks.
 *
 * GCN GPUs have no fixed-function 2D engine, so every drawing call is
 * rendered by GDI into a system-memory shadow of the screen and the touched
 * rectangle is then streamed to the write-combined framebuffer.  The GPU
 * framebuffer is never read: blits, ROPs, blending and ClearType all source
 * from cached system memory.
 */
#include "driver.h"

static __inline SURFOBJ *psoSubst(SURFOBJ *pso, PPDEV *pppdev)
{
    /* Our primary surface is the one whose dhsurf is its own PDEV. */
    if (pso != NULL && pso->iType == STYPE_DEVICE && pso->dhsurf != NULL &&
        (PVOID)pso->dhsurf == (PVOID)pso->dhpdev) {
        *pppdev = (PPDEV)pso->dhpdev;
        return (*pppdev)->psoShadow;
    }
    return pso;
}

/* Pushes a rectangle of the shadow to the screen. */
VOID vFlush(PPDEV ppdev, const RECTL *prcl, CLIPOBJ *pco)
{
    LONG l, t, r, b, y;
    ULONG cj;
    PBYTE src, dst;

    if (!ppdev->bEnabled)
        return;

    if (prcl != NULL) {
        l = min(prcl->left, prcl->right);
        r = max(prcl->left, prcl->right);
        t = min(prcl->top, prcl->bottom);
        b = max(prcl->top, prcl->bottom);
    } else {
        l = t = 0;
        r = ppdev->cx;
        b = ppdev->cy;
    }
    if (pco != NULL && pco->iDComplexity != DC_TRIVIAL) {
        l = max(l, pco->rclBounds.left);
        t = max(t, pco->rclBounds.top);
        r = min(r, pco->rclBounds.right);
        b = min(b, pco->rclBounds.bottom);
    }
    l = max(l, 0);
    t = max(t, 0);
    r = min(r, ppdev->cx);
    b = min(b, ppdev->cy);
    if (l >= r || t >= b)
        return;

    cj = (ULONG)(r - l) * ppdev->cjPixel;
    src = ppdev->pjShadow + t * ppdev->lDeltaShadow + l * (LONG)ppdev->cjPixel;
    dst = ppdev->pjScreen + t * ppdev->lDeltaScreen + l * (LONG)ppdev->cjPixel;
    for (y = t; y < b; y++) {
        __movsd((unsigned long *)dst, (const unsigned long *)src, cj >> 2);
        if (cj & 3)
            __movsb(dst + (cj & ~3u), src + (cj & ~3u), cj & 3);
        src += ppdev->lDeltaShadow;
        dst += ppdev->lDeltaScreen;
    }

    ppdev->stats.Flushes++;
    ppdev->stats.FlushKPixels += (ULONG)((r - l) * (b - t)) / 1000;
}

VOID vFlushAll(PPDEV ppdev)
{
    vFlush(ppdev, NULL, NULL);
}

/* Bounding box of a path, in whole pixels with a one-pixel margin. */
static VOID vPathBounds(PATHOBJ *ppo, RECTL *prcl)
{
    RECTFX rcfx;

    PATHOBJ_vGetBounds(ppo, &rcfx);
    prcl->left = (rcfx.xLeft >> 4) - 1;
    prcl->top = (rcfx.yTop >> 4) - 1;
    prcl->right = ((rcfx.xRight + 15) >> 4) + 2;
    prcl->bottom = ((rcfx.yBottom + 15) >> 4) + 2;
}

BOOL APIENTRY DrvBitBlt(SURFOBJ *psoTrg, SURFOBJ *psoSrc, SURFOBJ *psoMask,
                        CLIPOBJ *pco, XLATEOBJ *pxlo, RECTL *prclTrg,
                        POINTL *pptlSrc, POINTL *pptlMask, BRUSHOBJ *pbo,
                        POINTL *pptlBrush, ROP4 rop4)
{
    PPDEV ppdev = NULL, ppdevSrc = NULL;
    SURFOBJ *t = psoSubst(psoTrg, &ppdev);
    SURFOBJ *s = psoSubst(psoSrc, &ppdevSrc);
    BOOL b;

    b = EngBitBlt(t, s, psoMask, pco, pxlo, prclTrg, pptlSrc, pptlMask, pbo,
                  pptlBrush, rop4);
    if (ppdev != NULL) {
        ppdev->stats.Calls[GCN_CALL_BITBLT]++;
        vFlush(ppdev, prclTrg, pco);
    }
    return b;
}

BOOL APIENTRY DrvCopyBits(SURFOBJ *psoDest, SURFOBJ *psoSrc, CLIPOBJ *pco,
                          XLATEOBJ *pxlo, RECTL *prclDest, POINTL *pptlSrc)
{
    PPDEV ppdev = NULL, ppdevSrc = NULL;
    SURFOBJ *d = psoSubst(psoDest, &ppdev);
    SURFOBJ *s = psoSubst(psoSrc, &ppdevSrc);
    BOOL b;

    b = EngCopyBits(d, s, pco, pxlo, prclDest, pptlSrc);
    if (ppdev != NULL) {
        ppdev->stats.Calls[GCN_CALL_COPYBITS]++;
        vFlush(ppdev, prclDest, pco);
    }
    return b;
}

BOOL APIENTRY DrvTextOut(SURFOBJ *pso, STROBJ *pstro, FONTOBJ *pfo,
                         CLIPOBJ *pco, RECTL *prclExtra, RECTL *prclOpaque,
                         BRUSHOBJ *pboFore, BRUSHOBJ *pboOpaque,
                         POINTL *pptlOrg, MIX mix)
{
    PPDEV ppdev = NULL;
    SURFOBJ *t = psoSubst(pso, &ppdev);
    BOOL b;

    b = EngTextOut(t, pstro, pfo, pco, prclExtra, prclOpaque, pboFore,
                   pboOpaque, pptlOrg, mix);
    if (ppdev != NULL) {
        RECTL rcl = pstro->rclBkGround;

        ppdev->stats.Calls[GCN_CALL_TEXTOUT]++;
        if (prclOpaque != NULL) {
            rcl.left = min(rcl.left, prclOpaque->left);
            rcl.top = min(rcl.top, prclOpaque->top);
            rcl.right = max(rcl.right, prclOpaque->right);
            rcl.bottom = max(rcl.bottom, prclOpaque->bottom);
        }
        if (prclExtra != NULL) {
            /* zero-terminated list of extra rectangles (underlines etc.) */
            for (; prclExtra->left != prclExtra->right; prclExtra++) {
                rcl.left = min(rcl.left, prclExtra->left);
                rcl.top = min(rcl.top, prclExtra->top);
                rcl.right = max(rcl.right, prclExtra->right);
                rcl.bottom = max(rcl.bottom, prclExtra->bottom);
            }
        }
        vFlush(ppdev, &rcl, pco);
    }
    return b;
}

BOOL APIENTRY DrvStrokePath(SURFOBJ *pso, PATHOBJ *ppo, CLIPOBJ *pco,
                            XFORMOBJ *pxo, BRUSHOBJ *pbo,
                            POINTL *pptlBrushOrg, LINEATTRS *plineattrs,
                            MIX mix)
{
    PPDEV ppdev = NULL;
    SURFOBJ *t = psoSubst(pso, &ppdev);
    RECTL rcl;
    BOOL bGeometric = plineattrs != NULL && (plineattrs->fl & LA_GEOMETRIC);
    BOOL b;

    /* Take the bounds first: stroking may consume the path enumeration. */
    if (ppdev != NULL && !bGeometric)
        vPathBounds(ppo, &rcl);
    b = EngStrokePath(t, ppo, pco, pxo, pbo, pptlBrushOrg, plineattrs, mix);
    if (ppdev != NULL) {
        ppdev->stats.Calls[GCN_CALL_STROKEPATH]++;
        /* Wide lines extend past the path by an amount we don't compute. */
        vFlush(ppdev, bGeometric ? NULL : &rcl, pco);
    }
    return b;
}

BOOL APIENTRY DrvFillPath(SURFOBJ *pso, PATHOBJ *ppo, CLIPOBJ *pco,
                          BRUSHOBJ *pbo, POINTL *pptlBrushOrg, MIX mix,
                          FLONG flOptions)
{
    PPDEV ppdev = NULL;
    SURFOBJ *t = psoSubst(pso, &ppdev);
    RECTL rcl;
    BOOL b;

    if (ppdev != NULL)
        vPathBounds(ppo, &rcl);
    b = EngFillPath(t, ppo, pco, pbo, pptlBrushOrg, mix, flOptions);
    if (ppdev != NULL) {
        ppdev->stats.Calls[GCN_CALL_FILLPATH]++;
        vFlush(ppdev, &rcl, pco);
    }
    return b;
}

BOOL APIENTRY DrvStrokeAndFillPath(SURFOBJ *pso, PATHOBJ *ppo, CLIPOBJ *pco,
                                   XFORMOBJ *pxo, BRUSHOBJ *pboStroke,
                                   LINEATTRS *plineattrs, BRUSHOBJ *pboFill,
                                   POINTL *pptlBrushOrg, MIX mixFill,
                                   FLONG flOptions)
{
    PPDEV ppdev = NULL;
    SURFOBJ *t = psoSubst(pso, &ppdev);
    BOOL b;

    b = EngStrokeAndFillPath(t, ppo, pco, pxo, pboStroke, plineattrs, pboFill,
                             pptlBrushOrg, mixFill, flOptions);
    if (ppdev != NULL) {
        ppdev->stats.Calls[GCN_CALL_STROKEFILL]++;
        vFlush(ppdev, NULL, pco);
    }
    return b;
}

BOOL APIENTRY DrvLineTo(SURFOBJ *pso, CLIPOBJ *pco, BRUSHOBJ *pbo, LONG x1,
                        LONG y1, LONG x2, LONG y2, RECTL *prclBounds, MIX mix)
{
    PPDEV ppdev = NULL;
    SURFOBJ *t = psoSubst(pso, &ppdev);
    BOOL b;

    b = EngLineTo(t, pco, pbo, x1, y1, x2, y2, prclBounds, mix);
    if (ppdev != NULL) {
        RECTL rcl;

        ppdev->stats.Calls[GCN_CALL_LINETO]++;
        rcl.left = min(x1, x2) - 1;
        rcl.top = min(y1, y2) - 1;
        rcl.right = max(x1, x2) + 2;
        rcl.bottom = max(y1, y2) + 2;
        vFlush(ppdev, &rcl, pco);
    }
    return b;
}

BOOL APIENTRY DrvStretchBlt(SURFOBJ *psoDest, SURFOBJ *psoSrc,
                            SURFOBJ *psoMask, CLIPOBJ *pco, XLATEOBJ *pxlo,
                            COLORADJUSTMENT *pca, POINTL *pptlHTOrg,
                            RECTL *prclDest, RECTL *prclSrc, POINTL *pptlMask,
                            ULONG iMode)
{
    PPDEV ppdev = NULL, ppdevSrc = NULL;
    SURFOBJ *d = psoSubst(psoDest, &ppdev);
    SURFOBJ *s = psoSubst(psoSrc, &ppdevSrc);
    BOOL b;

    b = EngStretchBlt(d, s, psoMask, pco, pxlo, pca, pptlHTOrg, prclDest,
                      prclSrc, pptlMask, iMode);
    if (ppdev != NULL) {
        ppdev->stats.Calls[GCN_CALL_STRETCHBLT]++;
        vFlush(ppdev, prclDest, pco);
    }
    return b;
}

BOOL APIENTRY DrvStretchBltROP(SURFOBJ *psoDest, SURFOBJ *psoSrc,
                               SURFOBJ *psoMask, CLIPOBJ *pco, XLATEOBJ *pxlo,
                               COLORADJUSTMENT *pca, POINTL *pptlHTOrg,
                               RECTL *prclDest, RECTL *prclSrc,
                               POINTL *pptlMask, ULONG iMode, BRUSHOBJ *pbo,
                               DWORD rop4)
{
    PPDEV ppdev = NULL, ppdevSrc = NULL;
    SURFOBJ *d = psoSubst(psoDest, &ppdev);
    SURFOBJ *s = psoSubst(psoSrc, &ppdevSrc);
    BOOL b;

    b = EngStretchBltROP(d, s, psoMask, pco, pxlo, pca, pptlHTOrg, prclDest,
                         prclSrc, pptlMask, iMode, pbo, rop4);
    if (ppdev != NULL) {
        ppdev->stats.Calls[GCN_CALL_STRETCHROP]++;
        vFlush(ppdev, prclDest, pco);
    }
    return b;
}

BOOL APIENTRY DrvAlphaBlend(SURFOBJ *psoDest, SURFOBJ *psoSrc, CLIPOBJ *pco,
                            XLATEOBJ *pxlo, RECTL *prclDest, RECTL *prclSrc,
                            BLENDOBJ *pBlendObj)
{
    PPDEV ppdev = NULL, ppdevSrc = NULL;
    SURFOBJ *d = psoSubst(psoDest, &ppdev);
    SURFOBJ *s = psoSubst(psoSrc, &ppdevSrc);
    BOOL b;

    b = EngAlphaBlend(d, s, pco, pxlo, prclDest, prclSrc, pBlendObj);
    if (ppdev != NULL) {
        ppdev->stats.Calls[GCN_CALL_ALPHABLEND]++;
        vFlush(ppdev, prclDest, pco);
    }
    return b;
}

BOOL APIENTRY DrvTransparentBlt(SURFOBJ *psoDst, SURFOBJ *psoSrc,
                                CLIPOBJ *pco, XLATEOBJ *pxlo, RECTL *prclDst,
                                RECTL *prclSrc, ULONG iTransColor,
                                ULONG ulReserved)
{
    PPDEV ppdev = NULL, ppdevSrc = NULL;
    SURFOBJ *d = psoSubst(psoDst, &ppdev);
    SURFOBJ *s = psoSubst(psoSrc, &ppdevSrc);
    BOOL b;

    b = EngTransparentBlt(d, s, pco, pxlo, prclDst, prclSrc, iTransColor,
                          ulReserved);
    if (ppdev != NULL) {
        ppdev->stats.Calls[GCN_CALL_TRANSPARENT]++;
        vFlush(ppdev, prclDst, pco);
    }
    return b;
}

BOOL APIENTRY DrvGradientFill(SURFOBJ *psoDest, CLIPOBJ *pco, XLATEOBJ *pxlo,
                              TRIVERTEX *pVertex, ULONG nVertex, PVOID pMesh,
                              ULONG nMesh, RECTL *prclExtents,
                              POINTL *pptlDitherOrg, ULONG ulMode)
{
    PPDEV ppdev = NULL;
    SURFOBJ *d = psoSubst(psoDest, &ppdev);
    BOOL b;

    b = EngGradientFill(d, pco, pxlo, pVertex, nVertex, pMesh, nMesh,
                        prclExtents, pptlDitherOrg, ulMode);
    if (ppdev != NULL) {
        ppdev->stats.Calls[GCN_CALL_GRADIENT]++;
        vFlush(ppdev, prclExtents, pco);
    }
    return b;
}

BOOL APIENTRY DrvPlgBlt(SURFOBJ *psoTrg, SURFOBJ *psoSrc, SURFOBJ *psoMsk,
                        CLIPOBJ *pco, XLATEOBJ *pxlo, COLORADJUSTMENT *pca,
                        POINTL *pptlBrushOrg, POINTFIX *pptfx, RECTL *prcl,
                        POINTL *pptl, ULONG iMode)
{
    PPDEV ppdev = NULL, ppdevSrc = NULL;
    SURFOBJ *t = psoSubst(psoTrg, &ppdev);
    SURFOBJ *s = psoSubst(psoSrc, &ppdevSrc);
    BOOL b;

    b = EngPlgBlt(t, s, psoMsk, pco, pxlo, pca, pptlBrushOrg, pptfx, prcl,
                  pptl, iMode);
    if (ppdev != NULL) {
        ppdev->stats.Calls[GCN_CALL_PLGBLT]++;
        vFlush(ppdev, NULL, pco);
    }
    return b;
}
