# gcn4xp

A display driver for AMD GCN graphics cards (Polaris: RX 460–590) on
32-bit Windows XP.

Status: **2D desktop only.** No Direct3D, OpenGL, DirectDraw or video
acceleration. GPU compute is available separately through an HSA runtime
([hsa/](hsa/README.md)), with an SDK for writing GPU programs on XP itself
([sdk/](sdk/README.md)).

## What it does

| Component   | Role |
|-------------|------|
| `gcnmp.sys` | Video miniport. Claims the card, sets display modes through the card's VESA BIOS, maps the framebuffer write-combined, drives the display controller's hardware cursor. |
| `gcndd.dll` | GDI display driver. Renders into a system-memory shadow of the screen and streams dirty rectangles to video memory. |
| `gcninst.exe` | Command-line installer (works from a remote shell). |
| `gcnctl.exe`  | Diagnostics: adapter info, driver log, register dumps, mode switching, screenshots. |

### About "2D acceleration" on this hardware

GCN GPUs have no fixed-function 2D engine: there is no blitter to hand
`BitBlt` to. What makes the desktop fast here is:

- **Shadow framebuffer.** GDI never reads video memory. Scrolling, window
  moves, ROPs, alpha blending and ClearType all read cached system RAM, and
  only finished pixels are written to the card.
- **Write-combined video memory**, so those writes are streamed in bursts.
- **Hardware cursor** (64x64 ARGB, alpha-blended by the display controller).
  Pointers that invert the screen (e.g. the text I-beam) fall back to GDI's
  software cursor.

True GPU-side blits would go through the SDMA or 3D engine, which needs
firmware loading and GPU memory management. That is future work.

### Limitations

- Modes come from the card's VESA BIOS, so the list is whatever the BIOS
  offers; the machine must boot in legacy/CSM mode.
- The refresh rate is chosen by the BIOS (normally the monitor's preferred
  timing, e.g. 144 Hz on a 144 Hz panel) and cannot be selected. The driver
  measures it after each mode set and reports the measured value, so a
  mode's rate is only shown correctly once that mode has been used.
- Resolutions below the monitor's native one are stretched by the GPU
  scaler; the hardware cursor is not used in those modes.
- 16 and 32 bpp only.
- Single display pipeline as configured by the BIOS; no multi-monitor
  control, no EDID handling.
- Only tested on an RX 580 (1002:67DF). Other Polaris IDs are listed in the
  INF on the strength of sharing the same display controller.

## Building

Requirements: Visual Studio 2017 or later with the C++ x86 tools and a
Windows SDK. No DDK/WDK is needed — the kernel import libraries are
generated from `imports/*.imp`.

```powershell
.\build.ps1              # release build -> build\pkg
.\build.ps1 -DebugBuild  # also enables raw register writes via gcnctl
```

Do not ship debug builds: the register-write escape is reachable by any
local user.

## Installing

Copy `build\pkg` to the XP machine and run:

```
gcninst install gcn4xp.inf
```

then reboot. XP will not start a new driver on the adapter the VGA driver
is currently displaying on, so the device shows "cannot start" (Code 10)
until the reboot.

The driver is unsigned. `gcninst` honours the machine's driver signing
policy; set it to *Ignore* or *Warn* (System Properties → Hardware →
Driver Signing).

`scripts\deploy.ps1` automates copy + install over SSH for a test machine:

```powershell
$env:GCN_TARGET = 'user@host'
.\scripts\deploy.ps1 -Install -Reboot
```

To uninstall: `gcninst remove`, then reboot.

### If the screen stays black or the machine does not boot

Press F8 at boot and choose *Last Known Good Configuration* or *Safe Mode*
(safe mode never loads this driver), then run `gcninst remove`.

## Diagnostics

```
gcnctl info       adapter, memory layout, current mode
gcnctl log        miniport log (BIOS modes found, mode sets, mappings)
gcnctl dce        display controller state per CRTC
gcnctl stats      drawing call counters
gcnctl verify     compare the shadow with what is really in video memory
gcnctl modes      modes Windows offers
gcnctl setmode 1280 1024 32
```

The miniport also mirrors its log to the registry value `gcn.Log` under the
adapter's `HKLM\SYSTEM\CurrentControlSet\Control\Video\{...}\0000` key, which
is readable even when the display driver did not load.

## Layout

```
miniport/   gcnmp.sys   (vp.h is a minimal videoprt interface header)
display/    gcndd.dll
common/     runtime stubs shared by both
include/    interface shared by miniport, display driver and tools
imports/    export lists used to generate videoprt/win32k import libraries
inf/        gcn4xp.inf
tools/      gcninst, gcnctl
scripts/    deploy helper
hsa/        compute: engine bring-up, HSA runtime, code object loader
hsa/examples/  example GPU programs
sdk/        builds the SDK: clang/lld/lldb, make and the runtime for XP
```

Register definitions follow the Linux amdgpu register headers.
