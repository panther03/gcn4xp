# gcn4xp SDK

Builds `gcn4xp_sdk.zip`: everything needed to write GPU programs on the
Windows XP machine itself, with nothing else installed there.

| In the SDK | |
|------------|-|
| clang, lld 21.1.8 | C/C++ for 32-bit XP (MinGW-w64, `msvcrt`, static runtime) and AMDGPU code objects |
| lldb 21.1.8 | debugger |
| GNU make 4.4.1 | works from `cmd.exe`; needs no shell |
| HSA runtime | `lib/libhsa-runtime.a`, `include/hsa/`, the `gcngpu` and `hsatest` tools, GPU firmware |
| examples | [hsa/examples](../hsa/examples/README.md), building with the SDK alone |

`sdk-readme.txt` becomes the archive's `README.txt`: how to use it.

## Building the archive

On a current Windows machine, from a fresh clone, in MSYS2's or Git's shell:

```
sh sdk/prepare-sdk.sh
```

The only prerequisite is a 32-bit MinGW-w64 GCC with `msvcrt` and posix
threads: MSYS2's `mingw-w64-i686-gcc` and `mingw-w64-i686-binutils`,
expected in `/c/msys64/mingw32` (set `MINGW` otherwise). The script uses
`tar` and `curl` from Windows itself (Windows 10 1803 or later). It
downloads the rest into `build/sdk/` (untracked) and checks each against a
pinned SHA-256:

| Download | Why |
|----------|-----|
| LLVM 21.1.8 source | clang, lld, lldb |
| CMake 3.31.6, Ninja 1.12.1 | to build LLVM |
| Python 3.12.8 (embeddable) | LLVM's build needs it |
| GNU make 4.4.1 source | make for XP |
| linux-firmware 20261019: Polaris10 RLC and MEC | the runtime loads them |

A first run takes about 40 minutes on 12 threads; later runs only redo what
changed. The result is `build/gcn4xp_sdk.zip`. Extract it on XP (Explorer
can) and follow its README.

With `XP_DLLS=<dir>` the script also checks every program in the SDK
against XP's system DLLs (copied from XP's `system32`), see below.

## How the tools run on XP

LLVM's build and its Windows code target Windows 7. What it takes:

| Problem | Fix |
|---------|-----|
| LLVM calls a few Vista/Windows 7 functions directly (file paths from handles, rename/delete by handle, known folders, processor groups, `RegGetValueW`, `GetThreadId`, ...) | `patches/llvm/0001-*.patch` adds `XPCompat.h`: a stand-in per function that uses the real one when present and an XP equivalent otherwise. |
| Older functions called with Windows 7 flags, which XP rejects (`FindFirstFileExW` with `FindExInfoBasic`, `LoadLibraryExA` with `LOAD_LIBRARY_SEARCH_SYSTEM32`) | Same header: retry without the flags. The first broke directory listing, so clang could not find its own libraries. |
| `K32*` process status functions (Windows 7) | `PSAPI_VERSION 1`: the `psapi.dll` versions. |
| lld marks executables as needing Windows 6.0; XP's loader refuses them | OS and subsystem version 5.1, for the SDK's programs and, through the config file, for what clang builds. |
| clang's MinGW driver finds headers through a `gcc` on `PATH` | The config file pins `--sysroot` to the SDK. |
| GCC's `libstdc++` needs winpthreads, which gcc adds and clang does not | `-lpthread` in the config file. |
| A zip cannot hold the hard links LLVM's tool aliases normally are | `launcher.c`: a 20 KB stand-in per alias (`clang++`, `ld.lld`, ...) that starts the real tool with the option for that personality. |
| lldb: more Vista calls (`CancelIoEx`, process attribute lists, `QueryFullProcessImageNameW`, slim reader/writer locks, `msvcrt`'s `_s` functions, ...) | `patches/llvm/0002-*.patch`, mostly through the same stand-ins. On XP its pipe monitor polls, since XP cannot cancel another thread's read. |
| lldb never stopped at breakpoints: XP cannot name a process's main module before its loader runs, so lldb never registered the main thread | The same patch takes the name from the kernel (`GetProcessImageFileNameW`) instead. |
| Build paths, including the user name, end up in the programs (`__FILE__`, version banners with the git revision) | `-ffile-prefix-map` and `LLVM_APPEND_VC_REV=OFF`. The script refuses to package files that contain the repository path or the user's home directory. |

`config/i686-w64-windows-gnu.cfg` holds the defaults clang applies to
Windows programs (never to `--target=amdgcn-amd-amdhsa`). `build-llvm.sh`
and `build-make.sh` are the two builds, `launcher.c` the alias program.

### A GCC 15 miscompile

MSYS2's i686 GCC 15.2 miscompiles some 64-bit additions at `-O2`: it reuses
an address register after overwriting it, so the upper half of a 64-bit
operand is read from the wrong place (`gcc-check.c` has the details). It
corrupted the runtime's kernel entry addresses. GCC 15's new late combine
pass is responsible, so everything the script builds with GCC (LLVM, make,
the runtime, the launcher) gets `-fno-late-combine-instructions`. Before
building anything, the script compiles and runs `gcc-check.c` with that
flag and stops if the result is wrong. Code built by the SDK's clang is
not affected.

## Checking a program for XP

```
python3 sdk/check-imports.py <dir with XP's DLLs> program.exe ...
```

This reports every imported function that XP's DLLs do not export, and
header fields XP's loader refuses. It cannot see functions looked up at
run time, nor newer flag values passed to old functions; only running on
XP shows those.

## Limits

- The C++ library is GCC's libstdc++, not libc++. There is no
  compiler-rt, so no sanitizers or profiling runtime.
- `clang-cl` (MSVC mode) needs MSVC's headers and libraries, which are not
  included.
- Moving files to the recycle bin (an LLVM API nothing here uses) fails on
  XP.
