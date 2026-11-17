/*
 * driver.h - gcndd display driver private definitions.
 */
#ifndef GCNDD_DRIVER_H
#define GCNDD_DRIVER_H

#include <stddef.h>
#include <stdarg.h>
#include <intrin.h>
#include <windef.h>
#include <wingdi.h>
#include <winddi.h>
#include <winioctl.h>
#include <ntddvdeo.h>
#include "gcn_shared.h"

#define GCN_ALLOC_TAG 'dNCG'
#define GCN_DLL_NAME  L"gcndd"

typedef struct _PDEV {
    HANDLE   hDriver;         /* miniport device handle */
    HDEV     hdevEng;
    HSURF    hsurfDev;        /* device-managed primary surface */
    HSURF    hsurfShadow;     /* engine bitmap over the shadow buffer */
    SURFOBJ *psoShadow;
    HPALETTE hpalDefault;

    ULONG    ulMode;          /* miniport mode index */
    LONG     cx;
    LONG     cy;
    ULONG    cBitsPixel;
    ULONG    cjPixel;
    ULONG    iBitmapFormat;
    LONG     lDeltaScreen;    /* VRAM pitch in bytes */
    LONG     lDeltaShadow;
    FLONG    flRed;
    FLONG    flGreen;
    FLONG    flBlue;

    PBYTE    pjScreen;        /* write-combined VRAM mapping */
    PBYTE    pjShadow;        /* system-memory copy GDI renders into */
    BOOL     bEnabled;        /* FALSE while another mode owns the display */

    BOOL     bHwPointerCapable;
    BOOL     bHwPointer;      /* current shape lives in the hardware cursor */
    GCN_POINTER_POS ptr;
    GCN_POINTER_SHAPE shape;

    GCN_STATS stats;
} PDEV, *PPDEV;

/* screen.c */
BOOL bInitPDEV(PPDEV ppdev, DEVMODEW *pdm, GDIINFO *pgdi, DEVINFO *pdi);
DWORD cGetModes(HANDLE hDriver, PVIDEO_MODE_INFORMATION *ppModes);
BOOL bMiniport(PPDEV ppdev, DWORD code, PVOID in, DWORD cjIn, PVOID out,
               DWORD cjOut, DWORD *pcjRet);

/* draw.c */
VOID vFlush(PPDEV ppdev, const RECTL *prcl, CLIPOBJ *pco);
VOID vFlushAll(PPDEV ppdev);

/* pointer.c */
VOID vPointerDisable(PPDEV ppdev);
VOID vPointerRestore(PPDEV ppdev);

#endif
