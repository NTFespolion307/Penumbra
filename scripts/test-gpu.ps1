<#
.SYNOPSIS
  Runs the GPU tests (ctest label "gpu") after scripts\build.ps1 and prints a summary to paste into a report:
  the GPUs found, then each test's result. -Label picks the tests: all GPU tests (gpu, the default) or one
  backend's (vulkan, cuda; a GPU without ray tracing: cuda).

.EXAMPLE
  .\scripts\test-gpu.ps1
.EXAMPLE
  .\scripts\test-gpu.ps1 -Label cuda
#>
[CmdletBinding()]
param([ValidateSet('release', 'debug')] [string] $Config = 'release',
      [ValidateSet('gpu', 'vulkan', 'cuda')] [string] $Label = 'gpu')

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$preset = "windows-msvc-$Config"
$bin = Join-Path $root "build\$preset\bin\penumbra.exe"
if (-not (Test-Path $bin)) {
  Write-Host "ERROR: $bin not found; run scripts\build.ps1 first" -ForegroundColor Red
  exit 1
}
Write-Host "== $(& $bin --version | Select-Object -First 1)"
& $bin --list-devices
Write-Host "== GPU tests (label $Label)"
Set-Location $root
$log = Join-Path $root "build\$preset\gpu-tests.log"
ctest --preset $preset -L $Label --output-on-failure *> $log
$code = $LASTEXITCODE
Select-String -Path $log -Pattern 'Test +#|tests passed|tests failed|relMSE|Failed' | ForEach-Object { $_.Line }
exit $code
