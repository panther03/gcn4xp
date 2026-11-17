/*
 * gcn_shared.h - definitions shared by the miniport, the display driver
 * and the user-mode tools.
 */
#ifndef GCN_SHARED_H
#define GCN_SHARED_H

#define GCN_VERSION 0x00000100 /* 0.1.0 */

/* ---- private miniport IOCTLs (display driver -> miniport) -------------- */

#define GCN_IOCTL(n) \
    CTL_CODE(FILE_DEVICE_VIDEO, 0x900 + (n), METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_GCN_GET_INFO          GCN_IOCTL(0) /* out: GCN_INFO */
#define IOCTL_GCN_READ_LOG          GCN_IOCTL(1) /* out: text */
#define IOCTL_GCN_ALLOC_SHADOW      GCN_IOCTL(2) /* in: ULONG bytes, out: PVOID */
#define IOCTL_GCN_FREE_SHADOW       GCN_IOCTL(3) /* in: PVOID from ALLOC */
#define IOCTL_GCN_SET_POINTER_SHAPE GCN_IOCTL(4) /* in: GCN_POINTER_SHAPE */
#define IOCTL_GCN_SET_POINTER_POS   GCN_IOCTL(5) /* in: GCN_POINTER_POS */
#define IOCTL_GCN_REG_READ          GCN_IOCTL(6) /* in: GCN_REG_IO, out: ULONG[] */
#define IOCTL_GCN_REG_WRITE         GCN_IOCTL(7) /* in: GCN_REG_IO (debug builds) */

#define GCN_CURSOR_DIM 64

typedef struct _GCN_INFO {
    ULONG Version;
    ULONG VendorId;
    ULONG DeviceId;
    ULONG Revision;
    ULONG FbPhysLow;      /* aperture (BAR) base */
    ULONG FbPhysHigh;
    ULONG FbApertureLen;
    ULONG MmioPhysLow;
    ULONG MmioLen;
    ULONG VramMB;
    ULONG McFbBaseLow;    /* GPU address of VRAM start */
    ULONG McFbBaseHigh;
    ULONG VbeVersion;
    ULONG NumModes;
    LONG  CurrentMode;    /* -1 when no mode is set */
    ULONG Width;
    ULONG Height;
    ULONG Bpp;
    ULONG Pitch;          /* bytes */
    ULONG ModePhysLow;    /* physical address of the mode's framebuffer */
    ULONG CrtcMask;       /* CRTCs driven by the current mode */
    ULONG CursorOk;       /* hardware cursor usable */
    ULONG CursorOffset;   /* VRAM offset of the cursor image */
    ULONG WriteCombined;  /* framebuffer mapping got write-combining */
    ULONG DebugBuild;
} GCN_INFO;

typedef struct _GCN_POINTER_SHAPE {
    ULONG Width;          /* <= GCN_CURSOR_DIM */
    ULONG Height;
    ULONG Pixels[GCN_CURSOR_DIM * GCN_CURSOR_DIM]; /* premultiplied ARGB */
} GCN_POINTER_SHAPE;

typedef struct _GCN_POINTER_POS {
    LONG  X;              /* hot spot position on screen */
    LONG  Y;
    ULONG HotX;
    ULONG HotY;
    ULONG Visible;
} GCN_POINTER_POS;

typedef struct _GCN_REG_IO {
    ULONG Index;          /* register number (dword index) */
    ULONG Count;          /* read: number of registers */
    ULONG Value;          /* write: value */
} GCN_REG_IO;

/* ---- ExtEscape interface (tools -> display driver) --------------------- */

#define GCN_ESCAPE 0x4E4347 /* 'GCN' */

#define GCN_ESC_INFO     0   /* out: GCN_INFO */
#define GCN_ESC_LOG      1   /* out: text */
#define GCN_ESC_REG_READ 2   /* Arg0 = index, Arg1 = count, out: ULONG[] */
#define GCN_ESC_REG_WRITE 3  /* Arg0 = index, Arg1 = value (debug builds) */
#define GCN_ESC_STATS    4   /* out: GCN_STATS */
#define GCN_ESC_VERIFY   5   /* out: GCN_VERIFY - compare shadow and VRAM */

typedef struct _GCN_ESC_HEADER {
    ULONG Op;
    ULONG Arg0;
    ULONG Arg1;
} GCN_ESC_HEADER;

typedef struct _GCN_STATS {
    ULONG Flushes;        /* dirty rectangles pushed to VRAM */
    ULONG FlushKPixels;   /* thousands of pixels pushed */
    ULONG Calls[16];      /* per-hook call counts, see GCN_CALL_* */
    ULONG HwPointerSets;
    ULONG SwPointerSets;  /* shapes declined to GDI */
    ULONG PointerMoves;
} GCN_STATS;

#define GCN_CALL_BITBLT       0
#define GCN_CALL_COPYBITS     1
#define GCN_CALL_TEXTOUT      2
#define GCN_CALL_STROKEPATH   3
#define GCN_CALL_FILLPATH     4
#define GCN_CALL_STROKEFILL   5
#define GCN_CALL_LINETO       6
#define GCN_CALL_STRETCHBLT   7
#define GCN_CALL_STRETCHROP   8
#define GCN_CALL_ALPHABLEND   9
#define GCN_CALL_TRANSPARENT  10
#define GCN_CALL_GRADIENT     11
#define GCN_CALL_PLGBLT       12

typedef struct _GCN_VERIFY {
    ULONG BytesCompared;
    ULONG BytesDifferent;
    ULONG FirstDiffX;
    ULONG FirstDiffY;
} GCN_VERIFY;

#endif
