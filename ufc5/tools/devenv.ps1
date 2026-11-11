<#
.SYNOPSIS
    Set up a C++ build environment for KytyPS5 in the CURRENT shell.

.DESCRIPTION
    Copied from the PS4 UFC4 toolchain helper. Does NOT touch the system or user PATH.
    Everything it does lives and dies with this PowerShell session.

    It imports a real MSVC developer environment (PATH, INCLUDE, LIB, LIBPATH, and the
    Windows SDK vars) by invoking vcvars64.bat and copying the resulting variables into
    this session, then prepends ninja and LLVM/clang-cl.

    KytyPS5 requires clang-cl (not cl.exe). The Qt launcher is optional; the emulator
    itself builds with -DKYTY_BUILD_LAUNCHER=OFF.

.EXAMPLE
    . D:\PS5\tools\devenv.ps1
    Dot-source it so the variables land in your shell, not a child process.

.EXAMPLE
    . D:\PS5\tools\devenv.ps1 -Toolset BuildTools2022
#>
[CmdletBinding()]
param(
    [ValidateSet('Community2026', 'BuildTools2022')]
    [string]$Toolset = 'Community2026',

    # Extra directories to prepend to PATH for this session only.
    [string[]]$ExtraPath = @()
)

$ErrorActionPreference = 'Stop'

$Instances = @{
    'Community2026'  = 'D:\Program Files\Microsoft Visual Studio\18\Community'
    'BuildTools2022' = 'C:\BuildTools'
}

$vsRoot = $Instances[$Toolset]
if (-not (Test-Path -LiteralPath $vsRoot)) {
    throw "Visual Studio instance '$Toolset' not found at: $vsRoot"
}

$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "vcvars64.bat not found at: $vcvars"
}

Write-Host "==> Importing MSVC environment from $Toolset" -ForegroundColor Cyan
Write-Host "    $vcvars"

# vcvars64.bat shells out to vswhere.exe, which lives in the VS Installer directory and is
# not on PATH here. Without it vcvars still works but spews an error; give it the directory.
$installerDir = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer'
if ((Test-Path -LiteralPath $installerDir) -and ($env:PATH -notlike "*$installerDir*")) {
    $env:PATH = "$installerDir;$env:PATH"
}

# Run vcvars in cmd, then dump the resulting environment and absorb it.
$marker = '___VCVARS_ENV_BEGIN___'
$cmdLine = '"' + $vcvars + '" && echo ' + $marker + ' && set'
$raw = & "$env:ComSpec" /d /c $cmdLine

if ($LASTEXITCODE -ne 0) {
    throw "vcvars64.bat failed with exit code $LASTEXITCODE"
}

$seen = $false
$applied = 0
foreach ($line in $raw) {
    # cmd's `echo X && ...` emits a trailing space, so compare on the trimmed line.
    if (-not $seen) {
        if ($line.Trim() -eq $marker) { $seen = $true }
        continue
    }
    $eq = $line.IndexOf('=')
    if ($eq -lt 1) { continue }
    $name = $line.Substring(0, $eq)
    $value = $line.Substring($eq + 1)
    # Don't clobber shell bookkeeping vars.
    if ($name -in @('PROMPT', 'COMSPEC', 'PSModulePath')) { continue }
    Set-Item -Path "Env:\$name" -Value $value
    $applied++
}

if (-not $seen) { throw 'Failed to capture the vcvars environment (marker never appeared).' }
Write-Host "    imported $applied variables" -ForegroundColor DarkGray

# --- ninja -------------------------------------------------------------------
$ninjaCandidates = @(
    (Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'),
    'C:\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja',
    'D:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja',
    'C:\vcpkg\downloads\tools\ninja\1.10.2-windows'
)
$ninjaDir = $ninjaCandidates | Where-Object { Test-Path -LiteralPath (Join-Path $_ 'ninja.exe') } | Select-Object -First 1
if ($ninjaDir) {
    $env:PATH = "$ninjaDir;$env:PATH"
}

# --- clang / clang-cl --------------------------------------------------------
# Same LLVM 19.x install used by the PS4 shadPS4 builds.
$clangCandidates = @(
    'C:\Program Files\LLVM\bin',
    'D:\Program Files\LLVM\bin',
    (Join-Path $vsRoot 'VC\Tools\Llvm\x64\bin'),
    (Join-Path $vsRoot 'VC\Tools\Llvm\bin')
)
$clangDir = $clangCandidates | Where-Object { Test-Path -LiteralPath (Join-Path $_ 'clang-cl.exe') } | Select-Object -First 1
if ($clangDir) {
    $env:PATH = "$clangDir;$env:PATH"
}

# --- caller-supplied extras --------------------------------------------------
foreach ($p in $ExtraPath) {
    if (Test-Path -LiteralPath $p) { $env:PATH = "$p;$env:PATH" }
}

# --- report ------------------------------------------------------------------
Write-Host ''
Write-Host '==> Toolchain status' -ForegroundColor Cyan

function Show-Tool {
    param([string]$Name, [string]$VersionArg, [switch]$Required)

    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) {
        $ver = ''
        if ($VersionArg) {
            try { $ver = (& $cmd.Source $VersionArg 2>&1 | Select-Object -First 1) } catch { $ver = '' }
        }
        Write-Host ("  [OK]      {0,-10} {1}" -f $Name, $cmd.Source) -ForegroundColor Green
        if ($ver) { Write-Host ("                       {0}" -f $ver) -ForegroundColor DarkGray }
    }
    else {
        $colour = 'Yellow'
        $tag = '[absent]  '
        if ($Required) { $colour = 'Red'; $tag = '[MISSING] ' }
        Write-Host ("  {0}{1,-10} not on PATH" -f $tag, $Name) -ForegroundColor $colour
    }
}

Show-Tool -Name 'cl'       -VersionArg ''
Show-Tool -Name 'ninja'    -VersionArg '--version'
Show-Tool -Name 'cmake'    -VersionArg '--version'
Show-Tool -Name 'clang-cl' -VersionArg '--version' -Required
Show-Tool -Name 'git'      -VersionArg '--version'
Show-Tool -Name 'python'   -VersionArg '--version'

$kytyRoot = 'D:\PS5\src\KytyPS5'
$buildDir = Join-Path $kytyRoot '_Build\windows'

Write-Host ''
if (-not (Get-Command 'clang-cl' -ErrorAction SilentlyContinue)) {
    Write-Host '  clang-cl is required by KytyPS5 (MSVC cl.exe is not supported).' -ForegroundColor Yellow
    Write-Host '  Install LLVM 19.x or the Visual Studio "C++ Clang Compiler for Windows"' -ForegroundColor Yellow
    Write-Host '  component, then re-run this script. The PS4 tree uses the same compiler.' -ForegroundColor Yellow
    Write-Host ''
}
else {
    Write-Host '  Ready. Configure and build KytyPS5 with:' -ForegroundColor Green
    Write-Host "    cd $kytyRoot" -ForegroundColor DarkGray
    Write-Host '    cmake -S . -B _Build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DKYTY_BUILD_LAUNCHER=OFF' -ForegroundColor DarkGray
    Write-Host '    cmake --build _Build/windows --target kyty_emulator' -ForegroundColor DarkGray
    Write-Host ''
    Write-Host "  Build dir: $buildDir" -ForegroundColor DarkGray
    Write-Host '  Launcher is off (Qt not required). Turn it on later with -DKYTY_BUILD_LAUNCHER=ON' -ForegroundColor DarkGray
    Write-Host '  if you have a MSVC/clang-cl Qt 6 install.' -ForegroundColor DarkGray
    Write-Host ''
}
