<#
.SYNOPSIS
  Downloads the large sample scenes, which are too big for the repository, and checks every download against its
  SHA-256. Files already there and correct are kept, so running it again is quick.

.EXAMPLE
  .\scripts\get-scenes.ps1                       # Sponza (Khronos, ~58 MB) with an HDRI sky: scenes\sponza.pen.json
  .\scripts\get-scenes.ps1 -Scene intel-sponza   # Intel's New Sponza (4 GB download, 2.8 GB kept): scenes\intel-sponza.pen.json
#>
[CmdletBinding()]
param(
  [ValidateSet('sponza', 'intel-sponza')] [string] $Scene = 'sponza'
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'  # Windows PowerShell 5.1 downloads many times slower with the progress bar
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$root = Split-Path -Parent $PSScriptRoot

function Fail([string] $msg) {
  Write-Host "ERROR: $msg" -ForegroundColor Red
  exit 1
}
function Step([string] $msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Sha([string] $file) { (Get-FileHash $file -Algorithm SHA256).Hash.ToLower() }

# The Khronos Sponza: every file pinned to one commit, the sky from Poly Haven, and a courtyard camera added.
function Get-Sponza {
  $dir = Join-Path $root 'scenes\sponza'
  $manifest = Join-Path $root 'scenes\sponza.sha256'
  $khronos = 'https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/4995a638c96dbc94376d3d6164e70fee3ac7f7a2/Models/Sponza/glTF'
  $polyhaven = 'https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/2k'
  $keep = 166936  # bytes of Sponza.gltf before its node list; the camera version replaces the rest
  $ok = { param($hash, $name) $f = Join-Path $dir $name; (Test-Path $f) -and ((Sha $f) -eq $hash) }

  New-Item -ItemType Directory -Force $dir | Out-Null
  Step "Downloading Sponza and its sky into $dir"
  $entries = Get-Content $manifest | Where-Object { $_ -and -not $_.StartsWith('#') } | ForEach-Object {
    $hash, $name = $_ -split '\s+', 2
    [pscustomobject]@{ Hash = $hash; Name = $name }
  }
  $count = 0
  foreach ($e in $entries) {
    if ($e.Name -eq 'courtyard.gltf' -or (& $ok $e.Hash $e.Name)) { continue }
    $url = if ($e.Name.EndsWith('.hdr')) { "$polyhaven/$($e.Name)" } else { "$khronos/$($e.Name)" }
    try {
      Invoke-WebRequest -Uri $url -OutFile (Join-Path $dir $e.Name) -UseBasicParsing
    } catch {
      Fail "could not download $url ($($_.Exception.Message))"
    }
    if (-not (& $ok $e.Hash $e.Name)) { Fail "$($e.Name) does not match scenes\sponza.sha256 (damaged download?); run the script again" }
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
  if (-not (& $ok $want 'courtyard.gltf')) { Fail 'courtyard.gltf does not match scenes\sponza.sha256' }
  Write-Host 'Ready: scenes\sponza.pen.json' -ForegroundColor Green
}

# Intel's New Sponza (main scene): one 4 GB zip from Intel, checked, then only the glTF, its buffer, the textures
# and the HDRI are kept (the 3ds Max, FBX and USD versions are left out). CC BY 4.0, see the README.
function Get-IntelSponza {
  $dir = Join-Path $root 'scenes\intel-sponza'
  $url = 'https://cdrdv2.intel.com/v1/dl/getContent/830833'
  $want = 'b8bb853884ab1566b3beb35666bd09882a4e0dc16661e4684e103792cf0229b9'
  $zip = Join-Path $dir 'main_sponza.zip'
  $marker = Join-Path $dir ".extracted-$want"
  New-Item -ItemType Directory -Force $dir | Out-Null
  if (Test-Path $marker) {
    Write-Host 'Ready (already there): scenes\intel-sponza.pen.json' -ForegroundColor Green
    return
  }
  if (-not (Test-Path $zip) -or (Sha $zip) -ne $want) {
    Step "Downloading Intel's New Sponza (4 GB) into $dir"
    try {
      Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
    } catch {
      Fail "could not download $url ($($_.Exception.Message))"
    }
  }
  Step 'Checking the download'
  if ((Sha $zip) -ne $want) { Fail 'main_sponza.zip does not match its SHA-256 (damaged or changed download); delete it and run the script again' }
  Step 'Extracting the glTF version'
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  $archive = [IO.Compression.ZipFile]::OpenRead($zip)
  try {
    foreach ($entry in $archive.Entries) {
      $n = $entry.FullName
      $wanted = $n -eq 'main_sponza/NewSponza_Main_glTF_003.gltf' -or $n -eq 'main_sponza/NewSponza_Main_glTF_003.bin' -or
                $n -eq 'main_sponza/credits_license.txt' -or
                ($n.StartsWith('main_sponza/textures/') -and $entry.Name -and $entry.Name -ne 'desktop.ini')
      if (-not $wanted) { continue }
      $target = Join-Path $dir ($n -replace '/', '\')
      New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
      [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
    }
  } finally {
    $archive.Dispose()
  }
  Remove-Item $zip
  New-Item -ItemType File $marker | Out-Null
  Write-Host 'Ready: scenes\intel-sponza.pen.json' -ForegroundColor Green
}

if ($Scene -eq 'intel-sponza') { Get-IntelSponza; $pen = 'scenes\intel-sponza.pen.json' }
else { Get-Sponza; $pen = 'scenes\sponza.pen.json' }
Write-Host "Render it with: build\windows-msvc-release\bin\penumbra.exe --terminal $pen --spp 1024 -o render.png"
