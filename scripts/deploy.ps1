<#
.SYNOPSIS
    Copies build\pkg to a test machine over SSH and optionally installs it.

.DESCRIPTION
    The target is taken from -Target or the GCN_TARGET environment variable
    ([user@]host); the identity file from -Key or GCN_KEY (optional).  Nothing
    about the target is stored in the repository.

    The remote side is expected to be a Cygwin-style sshd on Windows XP.

.EXAMPLE
    $env:GCN_TARGET = 'user@testbox'; .\scripts\deploy.ps1 -Install
#>
param(
    [string]$Target = $env:GCN_TARGET,
    [string]$Key = $env:GCN_KEY,
    [string]$RemoteDir = '/cygdrive/c/gcn4xp',
    [switch]$Install,
    [switch]$Reboot
)

$ErrorActionPreference = 'Stop'
if (-not $Target) { throw 'No target: pass -Target or set GCN_TARGET.' }

$pkg = Join-Path (Split-Path $PSScriptRoot -Parent) 'build\pkg'
if (-not (Test-Path (Join-Path $pkg 'gcnmp.sys'))) { throw 'Run build.ps1 first.' }

$opts = @('-o', 'BatchMode=yes', '-o', 'ConnectTimeout=15')
if ($Key) { $opts += @('-i', $Key) }

function Remote([string]$Command) {
    & ssh @opts $Target $Command
    if ($LASTEXITCODE -ne 0) { throw "remote command failed: $Command" }
}

Remote "mkdir -p '$RemoteDir'"
Push-Location $pkg
try { & scp @opts -q @(Get-ChildItem -File -Name) "${Target}:$RemoteDir/" }
finally { Pop-Location }
if ($LASTEXITCODE -ne 0) { throw 'scp failed' }
Remote "chmod +x '$RemoteDir'/*.exe; ls -l '$RemoteDir'"

if ($Install) {
    Remote "cd '$RemoteDir' && ./gcninst.exe rescan && ./gcninst.exe install gcn4xp.inf; ./gcninst.exe status"
}
if ($Reboot) {
    Remote 'cmd /c "shutdown -r -t 0 -f"'
}
