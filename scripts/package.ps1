<#
.SYNOPSIS
  Builds the Windows release: dist\penumbra-windows-x64.zip (+ .sha256), Penumbra with the UI, ready to unzip and run.

.DESCRIPTION
  Same prerequisites as scripts\build.ps1. With the CUDA toolkit installed the release also has the CUDA backend;
  without it, Vulkan ray tracing and the CPU (and says so). Windows SmartScreen warns about a downloaded unsigned exe
  ("More info" > "Run anyway"); a build of your own (scripts\build.ps1) does not get that warning.

.EXAMPLE
  .\scripts\package.ps1
#>
[CmdletBinding()]
param()

# 'Continue', not 'Stop': Windows PowerShell 5.1 turns any stderr line of a native tool into a terminating error when
# output is redirected. Every native call is checked with Run instead.
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build\windows-package'
$stage = Join-Path $root 'dist\penumbra'
$out = Join-Path $root 'dist\penumbra-windows-x64.zip'

function Fail([string] $msg) {
  Write-Host "ERROR: $msg" -ForegroundColor Red
  exit 1
}
function Step([string] $msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Run([string] $what) {
  if ($LASTEXITCODE -ne 0) { Fail "$what failed (exit code $LASTEXITCODE)" }
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { Fail 'Visual Studio not found. See README.md "Prerequisites".' }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { Fail 'Visual Studio is installed without the C++ x64 tools ("Desktop development with C++").' }
# vcvars sets VCPKG_ROOT to the copy bundled with Visual Studio, so remember the user's choice first.
$userVcpkg = $env:VCPKG_ROOT
if (-not $userVcpkg) { $userVcpkg = [Environment]::GetEnvironmentVariable('VCPKG_ROOT', 'User') }
if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64' -or -not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
  $dump = cmd /c "`"$(Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat')`" >nul 2>&1 && set"
  if ($LASTEXITCODE -ne 0) { Fail 'Could not set up the x64 developer environment' }
  foreach ($line in $dump) {
    if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
  }
}
$env:VCPKG_ROOT = $userVcpkg
if (-not $env:VCPKG_ROOT -or -not (Test-Path (Join-Path $env:VCPKG_ROOT 'scripts\buildsystems\vcpkg.cmake'))) {
  Fail 'VCPKG_ROOT is not set to a vcpkg checkout (see README.md "Prerequisites").'
}
if (Get-Command nvcc -ErrorAction SilentlyContinue) { $cuda = 'ON' } else {
  $cuda = 'OFF'
  Write-Host '    no CUDA toolkit: the release has Vulkan ray tracing and the CPU only'
}

Push-Location $root
try {
  Step "Configuring a release build with the UI in $build"
  cmake --preset windows-msvc-release -B $build -DPEN_BUILD_TESTS=OFF "-DPEN_ENABLE_CUDA=$cuda"; Run 'Configure'
  Step 'Building'
  cmake --build $build; Run 'Build'

  $bin = Join-Path $build 'bin'
  Step 'Smoke test'
  & "$bin\penumbra.exe" --version; Run 'penumbra --version'
  & "$bin\penumbra.exe" --terminal scenes\cornell.pen.json --res 64x64 --spp 4 --backend cpu -o "$build\smoke.png" *> "$build\smoke.log"
  if ($LASTEXITCODE -ne 0) { Get-Content "$build\smoke.log"; Fail 'the smoke render failed' }

  Step "Staging $stage"
  if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
  New-Item -ItemType Directory -Force "$stage\scripts", "$stage\scenes", "$stage\licenses" | Out-Null
  Copy-Item "$bin\penumbra.exe", "$bin\penumbra-terrain.exe", "$bin\*.dll" $stage
  Copy-Item scripts\get-scenes.ps1 "$stage\scripts"
  Copy-Item scenes\*.pen.json, scenes\*.gltf, scenes\*.png, scenes\*.hdr, scenes\*.sha256, scenes\sponza-camera.txt "$stage\scenes"
  Copy-Item LICENSE "$stage\licenses\Penumbra-LICENSE.txt"
  $oidn = Get-ChildItem "$build\_deps\oidn-*\doc" -Directory | Select-Object -First 1
  Copy-Item "$($oidn.FullName)\LICENSE.txt" "$stage\licenses\OpenImageDenoise-LICENSE.txt"
  Copy-Item "$($oidn.FullName)\third-party-programs*.txt" "$stage\licenses"
  $version = (& "$bin\penumbra.exe" --version | Select-Object -First 1)
  @"
$version
Windows x64 release (UI, --terminal and --worker). Needs Windows 10 or 11 and a GPU driver: NVIDIA for Vulkan ray
tracing$(if ($cuda -eq 'ON') { ' and CUDA' }), or any Vulkan driver with ray tracing; else the CPU renders.

  penumbra.exe                                   (double-click: the UI)
  .\penumbra.exe --list-devices
  .\penumbra.exe --terminal scenes\cornell.pen.json --spp 256 -o cornell.png
  .\penumbra.exe --worker --join <controller address> --code <join code> --stay

Windows SmartScreen may warn about this downloaded exe: "More info" > "Run anyway", or build Penumbra yourself
(README "Build"). Every machine of a render must run the same version: download this release on all of them.
Sample scenes: .\scripts\get-scenes.ps1 (Sponza), .\scripts\get-scenes.ps1 -Scene swiss-alps (or swiss-lauterbrunnen,
swiss-matterhorn, swiss-rigi, or all of them: all).
Full documentation: https://github.com/NTFespolion307/Penumbra#readme
"@ | Set-Content -Encoding utf8 "$stage\README.txt"

  Step "Writing $out"
  if (Test-Path $out) { Remove-Item -Force $out }
  Compress-Archive -Path $stage -DestinationPath $out -CompressionLevel Optimal
  $hash = (Get-FileHash -Algorithm SHA256 $out).Hash.ToLower()
  "$hash  $(Split-Path -Leaf $out)" | Set-Content -Encoding ascii "$out.sha256"
  Write-Host "Built: $out ($([math]::Round((Get-Item $out).Length / 1MB, 1)) MB), $hash" -ForegroundColor Green
} finally {
  Pop-Location
}
