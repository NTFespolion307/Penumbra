<#
.SYNOPSIS
  Builds Penumbra on Windows: checks prerequisites, configures, builds, runs the tests and a smoke test,
  and prints where the binary is.

.EXAMPLE
  .\scripts\build.ps1                 # Release build with tests
  .\scripts\build.ps1 -Config debug   # Debug build
  .\scripts\build.ps1 -NoTests -Clean # Fresh build without running tests
#>
[CmdletBinding()]
param(
  [ValidateSet('release', 'debug')] [string] $Config = 'release',
  [switch] $NoTests,
  [switch] $Clean
)

# 'Continue', not 'Stop': Windows PowerShell 5.1 turns any stderr line of a native tool (CMake warnings)
# into a terminating error when output is redirected. Every native call is checked with Run instead.
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$preset = "windows-msvc-$Config"

function Fail([string] $msg) {
  Write-Host "ERROR: $msg" -ForegroundColor Red
  exit 1
}
function Step([string] $msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Run([string] $what) {
  # Call right after a native command: fails the build if it returned a non-zero exit code.
  if ($LASTEXITCODE -ne 0) { Fail "$what failed (exit code $LASTEXITCODE)" }
}

Step 'Checking prerequisites'

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
  Fail 'Git not found. Install it with: winget install --id Git.Git -e   (then open a new terminal)'
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
  Fail 'Visual Studio 2022 or 2026 (Build Tools are enough) not found. See README.md "Prerequisites".'
}
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) {
  Fail 'Visual Studio is installed without the C++ x64 tools. Add the "Desktop development with C++" workload.'
}

# Import the x64 developer environment (cl, link, ninja, Windows SDK) into this session. vcvars sets
# VCPKG_ROOT to the copy bundled with Visual Studio, so remember the user's choice first.
$userVcpkg = $env:VCPKG_ROOT
if (-not $userVcpkg) { $userVcpkg = [Environment]::GetEnvironmentVariable('VCPKG_ROOT', 'User') }
if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64' -or -not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
  $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
  $dump = cmd /c "`"$vcvars`" >nul 2>&1 && set"
  if ($LASTEXITCODE -ne 0) { Fail "Could not set up the x64 developer environment ($vcvars)" }
  foreach ($line in $dump) {
    if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
  }
}
Write-Host "    Visual Studio: $vs"

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) { Fail 'CMake not found. Install it with: winget install --id Kitware.CMake -e' }
$cmakeVersion = [version]((& cmake --version | Select-Object -First 1) -replace '^cmake version ([0-9]+\.[0-9]+).*$', '$1')
if ($cmakeVersion -lt [version]'3.26') { Fail "CMake 3.26 or newer is required, found $cmakeVersion" }
Write-Host "    CMake: $cmakeVersion ($($cmake.Source))"

if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
  Fail 'Ninja not found. Add the "C++ CMake tools for Windows" component in the Visual Studio Installer.'
}

$env:VCPKG_ROOT = $userVcpkg
if (-not $env:VCPKG_ROOT -or -not (Test-Path (Join-Path $env:VCPKG_ROOT 'scripts\buildsystems\vcpkg.cmake'))) {
  Fail 'vcpkg not found. Install it (README.md "Prerequisites") and set VCPKG_ROOT, for example C:\dev\vcpkg.'
}
Write-Host "    vcpkg: $env:VCPKG_ROOT"

# Parallel jobs: one per core, but at most one per 2 GB of available RAM (Embree's kernels need about 2 GB per
# compiler process; running out of memory makes vcpkg fall back to a single core).
$cores = [Environment]::ProcessorCount
if (-not $env:VCPKG_MAX_CONCURRENCY) {
  $freeGb = [math]::Floor((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1MB)
  $jobs = [math]::Max(1, [math]::Min($cores, [math]::Floor($freeGb / 2)))
  $env:VCPKG_MAX_CONCURRENCY = "$jobs"
  Write-Host "    parallel jobs: $jobs ($cores cores, $freeGb GB RAM available, ~2 GB per job; set VCPKG_MAX_CONCURRENCY to override)"
} else {
  Write-Host "    parallel jobs: $env:VCPKG_MAX_CONCURRENCY (from VCPKG_MAX_CONCURRENCY)"
}
if (-not $env:CMAKE_BUILD_PARALLEL_LEVEL) { $env:CMAKE_BUILD_PARALLEL_LEVEL = $env:VCPKG_MAX_CONCURRENCY }

Push-Location $root
try {
  if ($Clean -and (Test-Path "build\$preset")) {
    Step "Removing build\$preset"
    Remove-Item -Recurse -Force "build\$preset"
  }

  Step "Configuring ($preset); the first run builds the dependencies and takes a few minutes"
  cmake --preset $preset; Run 'Configure'

  Step 'Building'
  cmake --build --preset $preset; Run 'Build'

  if (-not $NoTests) {
    Step 'Running tests'
    ctest --preset $preset -LE 'large|gpu'; Run 'Tests'  # 20K gate: ctest -L large; GPU: scripts\test-gpu.ps1
  }

  $exe = Join-Path $root "build\$preset\bin\penumbra.exe"
  Step 'Smoke test'
  & $exe --version; Run 'penumbra --version'

  Write-Host ''
  Write-Host "Built: $exe" -ForegroundColor Green
} finally {
  Pop-Location
}
