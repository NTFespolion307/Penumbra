<#
.SYNOPSIS
  Downloads the large sample scene (Sponza with textures and an HDRI sky, about 58 MB) into scenes\sponza\,
  checks every file against scenes\sponza.sha256 and adds the courtyard camera. Files already there and
  correct are kept, so running it again is quick. Render it with: penumbra --terminal scenes\sponza.pen.json

.EXAMPLE
  .\scripts\get-scenes.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'  # Windows PowerShell 5.1 downloads many times slower with the progress bar
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$root = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $root 'scenes\sponza'
$manifest = Join-Path $root 'scenes\sponza.sha256'
# Pinned sources: the Khronos commit and the Poly Haven file never change, so the hashes stay valid.
$khronos = 'https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/4995a638c96dbc94376d3d6164e70fee3ac7f7a2/Models/Sponza/glTF'
$polyhaven = 'https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/2k'
$keep = 166936  # bytes of Sponza.gltf before its node list; the camera version replaces the rest

function Fail([string] $msg) {
  Write-Host "ERROR: $msg" -ForegroundColor Red
  exit 1
}
function Step([string] $msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Ok([string] $hash, [string] $name) {
  $f = Join-Path $dir $name
  (Test-Path $f) -and ((Get-FileHash $f -Algorithm SHA256).Hash.ToLower() -eq $hash)
}

New-Item -ItemType Directory -Force $dir | Out-Null
Step "Downloading Sponza and its sky into $dir"
$entries = Get-Content $manifest | Where-Object { $_ -and -not $_.StartsWith('#') } | ForEach-Object {
  $hash, $name = $_ -split '\s+', 2
  [pscustomobject]@{ Hash = $hash; Name = $name }
}
$count = 0
foreach ($e in $entries) {
  if ($e.Name -eq 'courtyard.gltf' -or (Ok $e.Hash $e.Name)) { continue }
  $url = if ($e.Name.EndsWith('.hdr')) { "$polyhaven/$($e.Name)" } else { "$khronos/$($e.Name)" }
  try {
    Invoke-WebRequest -Uri $url -OutFile (Join-Path $dir $e.Name) -UseBasicParsing
  } catch {
    Fail "could not download $url ($($_.Exception.Message))"
  }
  if (-not (Ok $e.Hash $e.Name)) { Fail "$($e.Name) does not match scenes\sponza.sha256 (damaged download?); run the script again" }
  $count++
}
Write-Host "    $count file(s) downloaded"

Step 'Adding the courtyard camera'
$gltf = [IO.File]::ReadAllBytes((Join-Path $dir 'Sponza.gltf'))
$camera = [IO.File]::ReadAllBytes((Join-Path $root 'scenes\sponza-camera.txt'))
$out = New-Object byte[] ($keep + $camera.Length)
[Array]::Copy($gltf, 0, $out, 0, $keep)
[Array]::Copy($camera, 0, $out, $keep, $camera.Length)
[IO.File]::WriteAllBytes((Join-Path $dir 'courtyard.gltf'), $out)
$want = ($entries | Where-Object { $_.Name -eq 'courtyard.gltf' }).Hash
if (-not (Ok $want 'courtyard.gltf')) { Fail 'courtyard.gltf does not match scenes\sponza.sha256' }

Write-Host 'Ready: scenes\sponza.pen.json' -ForegroundColor Green
Write-Host 'Render it with: build\windows-msvc-release\bin\penumbra.exe --terminal scenes\sponza.pen.json --spp 1024 -o sponza.png'
