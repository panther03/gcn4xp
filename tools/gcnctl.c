/*
 * gcnctl - diagnostics for the gcn4xp driver.
 *
 *   gcnctl info                 adapter and current mode
 *   gcnctl log                  miniport log
 *   gcnctl stats                display driver counters
 *   gcnctl verify               compare the shadow buffer with video memory
 *   gcnctl dce                  dump display controller state per CRTC
 *   gcnctl reg <index> [count]  read registers
 *   gcnctl regw <index> <value> write a register (debug builds only)
 *   gcnctl modes                list modes Windows offers
 *   gcnctl setmode <w> <h> <bpp>
 *   gcnctl shot <file.bmp>      save a screenshot
 *   gcnctl paint                draw a test pattern through GDI
 *
 * Works from a service or remote shell: it attaches itself to the
 * interactive window station before talking to the display.
 */
#include "tool.h"
#include <winioctl.h>
#include "gcn_shared.h"

static HDC g_hdc;

static void AttachInteractive(void)
{
    HWINSTA ws = OpenWindowStationA("WinSta0", FALSE, MAXIMUM_ALLOWED);
    HDESK desk;

    if (ws == NULL || !SetProcessWindowStation(ws))
        return;
    desk = OpenDesktopA("Default", 0, FALSE, MAXIMUM_ALLOWED);
    if (desk != NULL)
        SetThreadDesktop(desk);
}

static int Esc(ULONG op, ULONG a0, ULONG a1, void *out, int cbOut)
{
    GCN_ESC_HEADER hdr;

    hdr.Op = op;
    hdr.Arg0 = a0;
    hdr.Arg1 = a1;
    return ExtEscape(g_hdc, GCN_ESCAPE, sizeof(hdr), (LPCSTR)&hdr, cbOut,
                     (LPSTR)out);
}

static int Info(void)
{
    GCN_INFO i;

    if (Esc(GCN_ESC_INFO, 0, 0, &i, sizeof(i)) <= 0) {
        Print("info query failed\n");
        return 1;
    }
    Print("driver version  %lx%s\n", i.Version,
          i.DebugBuild ? " (debug build)" : "");
    Print("device          %04lx:%04lx rev %02lx\n", i.VendorId, i.DeviceId,
          i.Revision);
    Print("vram            %lu MB\n", i.VramMB);
    Print("fb aperture     %08lx%08lx, %lu MB\n", i.FbPhysHigh, i.FbPhysLow,
          i.FbApertureLen >> 20);
    Print("registers       %08lx, %lu KB\n", i.MmioPhysLow, i.MmioLen >> 10);
    Print("gpu fb base     %02lx%08lx\n", i.McFbBaseHigh, i.McFbBaseLow);
    Print("vbe             %lx.%lx, %lu modes\n", i.VbeVersion >> 8,
          i.VbeVersion & 0xFF, i.NumModes);
    if (i.CurrentMode >= 0)
        Print("mode            #%ld %lux%lux%lu pitch %lu at %08lx\n",
              i.CurrentMode, i.Width, i.Height, i.Bpp, i.Pitch,
              i.ModePhysLow);
    else
        Print("mode            none\n");
    Print("crtc mask       %lx\n", i.CrtcMask);
    Print("write combining %s\n", i.WriteCombined ? "yes" : "no");
    Print("hw cursor       %s (image at vram+%08lx)\n",
          i.CursorOk ? "available" : "unavailable", i.CursorOffset);
    return 0;
}

static int Log(void)
{
    static char buf[8192];
    int n = Esc(GCN_ESC_LOG, 0, 0, buf, sizeof(buf));

    if (n <= 0) {
        Print("log query failed\n");
        return 1;
    }
    PrintRaw(buf, (DWORD)n);
    return 0;
}

static int Stats(void)
{
    static const char *names[] = {
        "BitBlt", "CopyBits", "TextOut", "StrokePath", "FillPath",
        "StrokeAndFillPath", "LineTo", "StretchBlt", "StretchBltROP",
        "AlphaBlend", "TransparentBlt", "GradientFill", "PlgBlt"
    };
    GCN_STATS s;
    int i;

    if (Esc(GCN_ESC_STATS, 0, 0, &s, sizeof(s)) <= 0) {
        Print("stats query failed\n");
        return 1;
    }
    for (i = 0; i < 13; i++)
        Print("%-18s %lu\n", names[i], s.Calls[i]);
    Print("flushes            %lu (%lu kpixels)\n", s.Flushes,
          s.FlushKPixels);
    Print("pointer            %lu hardware shapes, %lu software, %lu moves\n",
          s.HwPointerSets, s.SwPointerSets, s.PointerMoves);
    return 0;
}

static int Verify(void)
{
    GCN_VERIFY v;

    if (Esc(GCN_ESC_VERIFY, 0, 0, &v, sizeof(v)) <= 0) {
        Print("verify query failed\n");
        return 1;
    }
    Print("%lu bytes compared, %lu differ", v.BytesCompared,
          v.BytesDifferent);
    if (v.BytesDifferent)
        Print(" (first at %lu,%lu)", v.FirstDiffX, v.FirstDiffY);
    Print("\n");
    return v.BytesDifferent || !v.BytesCompared ? 1 : 0;
}

static int RegRead(ULONG index, ULONG count)
{
    static ULONG vals[256];
    ULONG i;
    int n;

    if (count == 0)
        count = 1;
    if (count > 256)
        count = 256;
    n = Esc(GCN_ESC_REG_READ, index, count, vals, (int)(count * 4));
    if (n <= 0) {
        Print("register read failed\n");
        return 1;
    }
    for (i = 0; i < (ULONG)n / 4; i++)
        Print("%04lx: %08lx\n", index + i, vals[i]);
    return 0;
}

static ULONG Reg1(ULONG index)
{
    ULONG v = 0xDEADBEEF;

    Esc(GCN_ESC_REG_READ, index, 1, &v, sizeof(v));
    return v;
}

static int Dce(void)
{
    static const ULONG offs[6] = { 0, 0x200, 0x400, 0x2600, 0x2800, 0x2a00 };
    int i;

    Print("MC_VM_FB_LOCATION %08lx  CONFIG_MEMSIZE %lu MB\n", Reg1(0x809),
          Reg1(0x150a));
    for (i = 0; i < 6; i++) {
        ULONG o = offs[i];

        Print("crtc %d: control %08lx blank %08lx h_total %lu v_total %lu\n",
              i, Reg1(0x1b9c + o), Reg1(0x1b9d + o), Reg1(0x1b80 + o) + 1,
              Reg1(0x1b87 + o) + 1);
        if (!(Reg1(0x1b9c + o) & 1))
            continue;
        Print("  grph: enable %lx control %08lx surface %02lx%08lx pitch %lu"
              " x_end %lu y_end %lu\n",
              Reg1(0x1a00 + o), Reg1(0x1a01 + o), Reg1(0x1a07 + o),
              Reg1(0x1a04 + o), Reg1(0x1a06 + o), Reg1(0x1a0d + o),
              Reg1(0x1a0e + o));
        Print("  viewport: start %08lx size %08lx\n", Reg1(0x1b5c + o),
              Reg1(0x1b5d + o));
        Print("  cursor: control %08lx surface %02lx%08lx size %08lx"
              " pos %08lx hot %08lx\n",
              Reg1(0x1a66 + o), Reg1(0x1a69 + o), Reg1(0x1a67 + o),
              Reg1(0x1a68 + o), Reg1(0x1a6a + o), Reg1(0x1a6b + o));
    }
    return 0;
}

static int Modes(void)
{
    DEVMODEA dm;
    DWORD i;

    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm))
        Print("current: %lux%lux%lu @%lu\n", dm.dmPelsWidth, dm.dmPelsHeight,
              dm.dmBitsPerPel, dm.dmDisplayFrequency);
    for (i = 0; EnumDisplaySettingsA(NULL, i, &dm); i++)
        Print("%lux%lux%lu @%lu\n", dm.dmPelsWidth, dm.dmPelsHeight,
              dm.dmBitsPerPel, dm.dmDisplayFrequency);
    return 0;
}

static int Devices(void)
{
    DISPLAY_DEVICEA dd;
    DWORD i;

    ZeroMemory(&dd, sizeof(dd));
    dd.cb = sizeof(dd);
    for (i = 0; EnumDisplayDevicesA(NULL, i, &dd, 0); i++) {
        DEVMODEA dm;
        DWORD m, count = 0;

        ZeroMemory(&dm, sizeof(dm));
        dm.dmSize = sizeof(dm);
        for (m = 0; EnumDisplaySettingsA(dd.DeviceName, m, &dm); m++)
            count++;
        Print("%s: %s\n  flags %08lx%s%s%s, %lu modes\n", dd.DeviceName,
              dd.DeviceString, dd.StateFlags,
              (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)
                  ? " attached" : "",
              (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? " primary"
                                                              : "",
              (dd.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) ? " mirror"
                                                                : "",
              count);
    }
    return 0;
}

/* Sets a mode on a specific display device (e.g. \\.\DISPLAY1). */
static int SetModeOn(const char *device, DWORD w, DWORD h, DWORD bpp)
{
    DEVMODEA dm;
    LONG r;

    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    dm.dmPelsWidth = w;
    dm.dmPelsHeight = h;
    dm.dmBitsPerPel = bpp;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
    r = ChangeDisplaySettingsExA(device, &dm, NULL,
                                 CDS_UPDATEREGISTRY | CDS_SET_PRIMARY, NULL);
    Print("ChangeDisplaySettingsEx(%s) returned %ld%s\n", device, r,
          r == DISP_CHANGE_SUCCESSFUL ? " (ok)" : "");
    return r == DISP_CHANGE_SUCCESSFUL ? 0 : 1;
}

static int SetMode(DWORD w, DWORD h, DWORD bpp)
{
    DEVMODEA dm;
    LONG r;

    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    dm.dmPelsWidth = w;
    dm.dmPelsHeight = h;
    dm.dmBitsPerPel = bpp;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
    r = ChangeDisplaySettingsA(&dm, CDS_UPDATEREGISTRY);
    Print("ChangeDisplaySettings returned %ld%s\n", r,
          r == DISP_CHANGE_SUCCESSFUL ? " (ok)" : "");
    return r == DISP_CHANGE_SUCCESSFUL ? 0 : 1;
}

static int Shot(const char *file)
{
    int w = GetDeviceCaps(g_hdc, HORZRES), h = GetDeviceCaps(g_hdc, VERTRES);
    BITMAPINFO bi;
    BITMAPFILEHEADER fh;
    HDC mem = CreateCompatibleDC(g_hdc);
    HBITMAP bmp;
    void *bits = NULL;
    HANDLE f;
    DWORD cb, written;

    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    cb = (DWORD)(((w * 3 + 3) & ~3) * h);
    bmp = CreateDIBSection(g_hdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (mem == NULL || bmp == NULL)
        return 1;
    SelectObject(mem, bmp);
    if (!BitBlt(mem, 0, 0, w, h, g_hdc, 0, 0, SRCCOPY)) {
        Print("BitBlt failed\n");
        return 1;
    }
    GdiFlush();

    ZeroMemory(&fh, sizeof(fh));
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
    fh.bfSize = fh.bfOffBits + cb;
    f = CreateFileA(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        Print("cannot create %s\n", file);
        return 1;
    }
    WriteFile(f, &fh, sizeof(fh), &written, NULL);
    WriteFile(f, &bi.bmiHeader, sizeof(bi.bmiHeader), &written, NULL);
    WriteFile(f, bits, cb, &written, NULL);
    CloseHandle(f);
    Print("%dx%d screenshot written to %s\n", w, h, file);
    return 0;
}

/* Exercises the main drawing paths so stats / verify have something to see. */
static int Paint(void)
{
    int w = GetDeviceCaps(g_hdc, HORZRES), h = GetDeviceCaps(g_hdc, VERTRES);
    static const COLORREF cols[] = { RGB(255, 0, 0), RGB(0, 255, 0),
                                     RGB(0, 0, 255), RGB(255, 255, 255) };
    TRIVERTEX tv[2];
    GRADIENT_RECT gr = { 0, 1 };
    RECT rc;
    int i;

    for (i = 0; i < 4; i++) {
        HBRUSH br = CreateSolidBrush(cols[i]);
        SetRect(&rc, i * w / 4, 0, (i + 1) * w / 4, h / 4);
        FillRect(g_hdc, &rc, br);
        DeleteObject(br);
    }
    tv[0].x = 0;      tv[0].y = h / 4;
    tv[0].Red = 0;    tv[0].Green = 0;      tv[0].Blue = 0;  tv[0].Alpha = 0;
    tv[1].x = w;      tv[1].y = h / 2;
    tv[1].Red = 0xFF00; tv[1].Green = 0xFF00; tv[1].Blue = 0xFF00;
    tv[1].Alpha = 0;
    GradientFill(g_hdc, tv, 2, &gr, 1, GRADIENT_FILL_RECT_H);
    for (i = 0; i < 16; i++) {
        MoveToEx(g_hdc, 0, h / 2, NULL);
        LineTo(g_hdc, w - 1, h / 2 + i * (h / 32));
    }
    SetBkColor(g_hdc, RGB(0, 0, 64));
    SetTextColor(g_hdc, RGB(255, 255, 0));
    TextOutA(g_hdc, 16, h / 2 + 16, "gcn4xp test pattern", 19);
    Ellipse(g_hdc, w / 2, h / 2, w - 16, h - 16);
    BitBlt(g_hdc, 0, 3 * h / 4, w / 2, h / 4, g_hdc, 0, 0, SRCCOPY);
    GdiFlush();
    Print("test pattern drawn on %dx%d\n", w, h);
    return 0;
}

int ToolMain(int argc, char **argv)
{
    const char *cmd = argc >= 2 ? argv[1] : "";
    ULONG q = GCN_ESCAPE;

    AttachInteractive();

    if (StrEq(cmd, "modes"))
        return Modes();
    if (StrEq(cmd, "devices"))
        return Devices();
    if (StrEq(cmd, "setmode") && argc >= 6)
        return SetModeOn(argv[5], ParseNum(argv[2]), ParseNum(argv[3]),
                         ParseNum(argv[4]));
    if (StrEq(cmd, "setmode") && argc >= 5)
        return SetMode(ParseNum(argv[2]), ParseNum(argv[3]),
                       ParseNum(argv[4]));

    g_hdc = CreateDCA("DISPLAY", NULL, NULL, NULL);
    if (g_hdc == NULL) {
        Print("cannot open the display\n");
        return 1;
    }
    if (StrEq(cmd, "shot") && argc >= 3)
        return Shot(argv[2]);
    if (StrEq(cmd, "paint"))
        return Paint();

    if (ExtEscape(g_hdc, QUERYESCSUPPORT, sizeof(q), (LPCSTR)&q, 0, NULL) <=
        0) {
        Print("the gcn4xp display driver is not driving the display\n");
        return 1;
    }
    if (StrEq(cmd, "info"))
        return Info();
    if (StrEq(cmd, "log"))
        return Log();
    if (StrEq(cmd, "stats"))
        return Stats();
    if (StrEq(cmd, "verify"))
        return Verify();
    if (StrEq(cmd, "dce"))
        return Dce();
    if (StrEq(cmd, "reg") && argc >= 3)
        return RegRead(ParseNum(argv[2]), argc >= 4 ? ParseNum(argv[3]) : 1);
    if (StrEq(cmd, "regw") && argc >= 4) {
        if (Esc(GCN_ESC_REG_WRITE, ParseNum(argv[2]), ParseNum(argv[3]), NULL,
                0) <= 0) {
            Print("register write refused (needs a debug build)\n");
            return 1;
        }
        return 0;
    }
    Print("usage: gcnctl info|log|stats|verify|dce|modes|paint\n"
          "       gcnctl reg <index> [count] | regw <index> <value>\n"
          "       gcnctl setmode <w> <h> <bpp> | shot <file.bmp>\n");
    return 2;
}
