<#
.SYNOPSIS
    Builds the gcn4xp driver package into build\pkg.

.DESCRIPTION
    Needs Visual Studio (2017 or later) with the x86 C++ tools and a Windows
    SDK.  No DDK/WDK is required: import libraries for videoprt.sys and
    win32k.sys are generated from the lists in imports\.

.PARAMETER DebugBuild
    Enables diagnostics that must not ship (raw register writes via escape).
#>
param(
    [switch]$DebugBuild,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$out = Join-Path $root 'build'
$obj = Join-Path $out 'obj'
$imp = Join-Path $out 'imp'
$pkg = Join-Path $out 'pkg'

if ($Clean -and (Test-Path $out)) { Remove-Item -Recurse -Force $out }
foreach ($d in $obj, $imp, $pkg) { New-Item -ItemType Directory -Force $d | Out-Null }

# ---- toolchain -------------------------------------------------------------

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw 'Visual Studio not found (vswhere.exe missing).' }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'No Visual Studio installation with the C++ x86 tools found.' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
    cmd /c "`"$vcvars`" x86 >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
    }
}

function Invoke-Tool {
    param([string]$Exe, [string[]]$Arguments)
    & $Exe @Arguments | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed with exit code $LASTEXITCODE" }
}

# ---- import libraries ------------------------------------------------------

# Builds an import library for a kernel module from "<name> <argbytes>" lines
# by linking a stub DLL that exports stdcall functions under plain names.
function New-ImportLib {
    param([string]$Module, [string]$Spec)
    $name = [IO.Path]::GetFileNameWithoutExtension($Module)
    $c = @()
    $def = @("LIBRARY $Module", 'EXPORTS')
    foreach ($line in Get-Content $Spec) {
        $line = $line.Trim()
        if ($line -eq '' -or $line.StartsWith('#')) { continue }
        $fn, $bytes = $line -split '\s+'
        $n = [int]$bytes / 4
        if ($n -eq 0) { $params = 'void' }
        else { $params = (1..$n | ForEach-Object { "int a$_" }) -join ', ' }
        $c += "void __stdcall $fn($params) {}"
        $def += "    $fn"
    }
    $cFile = Join-Path $imp "$name.c"
    $defFile = Join-Path $imp "$name.def"
    Set-Content -Path $cFile -Value $c -Encoding ascii
    Set-Content -Path $defFile -Value $def -Encoding ascii
    Invoke-Tool cl.exe @('/nologo', '/c', '/Zl', '/GS-', '/W0', "/Fo$imp\$name.obj", $cFile)
    Invoke-Tool link.exe @('/nologo', '/DLL', '/NOENTRY', '/NODEFAULTLIB', '/MACHINE:X86',
        "/DEF:$defFile", "/OUT:$imp\$Module", "/IMPLIB:$imp\$name.lib", "$imp\$name.obj")
}

New-ImportLib 'videoprt.sys' (Join-Path $root 'imports\videoprt.imp')
New-ImportLib 'win32k.sys' (Join-Path $root 'imports\win32k.imp')
New-ImportLib 'ntoskrnl.exe' (Join-Path $root 'imports\ntoskrnl.imp')

# ---- compile ---------------------------------------------------------------

$cflags = @('/nologo', '/c', '/W3', '/O2', '/Oi', '/GS-', '/GF', '/Gy', '/Zl',
    '/Gs1000000', '/D_X86_=1', '/DWINVER=0x0501', '/D_WIN32_WINNT=0x0501',
    "/I$root\include")
if ($DebugBuild) { $cflags += '/DGCN_DEBUG' }

# Kernel code must not touch FPU/SSE state.
$kflags = $cflags + @('/arch:IA32', '/kernel')

function Compile {
    param([string[]]$Flags, [string]$Group, [string[]]$Sources)
    $dir = Join-Path $obj $Group
    New-Item -ItemType Directory -Force $dir | Out-Null
    $objs = @()
    foreach ($s in $Sources) {
        $o = Join-Path $dir ([IO.Path]::GetFileNameWithoutExtension($s) + '.obj')
        Invoke-Tool cl.exe ($Flags + @("/Fo$o", (Join-Path $root $s)))
        $objs += $o
    }
    return $objs
}

$lflags = @('/nologo', '/NODEFAULTLIB', '/MACHINE:X86', '/INCREMENTAL:NO',
    '/OPT:REF', '/OPT:ICF', '/RELEASE', '/SAFESEH:NO')

$mp = Compile $kflags 'miniport' @('miniport\gcnmp.c', 'common\nocrt.c')
$mplibs = @("$imp\videoprt.lib")
if ($DebugBuild) { $mplibs += "$imp\ntoskrnl.lib" }   # debug registry trace
Invoke-Tool link.exe ($lflags + @('/DRIVER', '/SUBSYSTEM:NATIVE,5.01',
    '/ENTRY:DriverEntry@8', "/OUT:$pkg\gcnmp.sys") + $mplibs + $mp)

$dd = Compile $kflags 'display' @('display\enable.c', 'display\screen.c',
    'display\draw.c', 'display\pointer.c', 'common\nocrt.c')
Invoke-Tool link.exe ($lflags + @('/DLL', '/DRIVER', '/SUBSYSTEM:NATIVE,5.01',
    '/ENTRY:DrvEnableDriver@12', "/OUT:$pkg\gcndd.dll", "$imp\win32k.lib") + $dd)

$syslibs = @('kernel32.lib', 'user32.lib', 'gdi32.lib', 'advapi32.lib',
    'setupapi.lib', 'cfgmgr32.lib', 'newdev.lib', 'msimg32.lib')
foreach ($tool in 'gcninst', 'gcnctl') {
    $t = Compile $cflags $tool @("tools\$tool.c", 'common\nocrt.c')
    Invoke-Tool link.exe ($lflags + @('/SUBSYSTEM:CONSOLE,5.01', '/ENTRY:ToolEntry',
        '/DYNAMICBASE:NO', '/NXCOMPAT:NO',
        "/OUT:$pkg\$tool.exe") + $syslibs + $t)
}

Copy-Item (Join-Path $root 'inf\gcn4xp.inf') $pkg -Force
Get-ChildItem $pkg -File | Where-Object { $_.Extension -in '.lib', '.exp' } | Remove-Item -Force

Write-Host "`nPackage ready in $pkg"
Get-ChildItem $pkg | Format-Table Name, Length -AutoSize
