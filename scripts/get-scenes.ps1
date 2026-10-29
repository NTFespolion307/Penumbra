<#
.SYNOPSIS
  Downloads the large sample scenes, which are too big for the repository, and checks every download against its
  SHA-256. Files already there and correct are kept, so running it again is quick.

.EXAMPLE
  .\scripts\get-scenes.ps1                       # Sponza (Khronos, ~58 MB) with an HDRI sky: scenes\sponza.pen.json
  .\scripts\get-scenes.ps1 -Scene intel-sponza   # Intel's New Sponza (4 GB download, 2.8 GB kept): scenes\intel-sponza.pen.json
  .\scripts\get-scenes.ps1 -Scene swiss-alps     # Eiger, Moench, Jungfrau (swisstopo, 330 MB; needs Penumbra built)
#>
[CmdletBinding()]
param(
  [ValidateSet('sponza', 'intel-sponza', 'swiss-alps')] [string] $Scene = 'sponza'
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

# The Swiss Alps (M14): Eiger, Moench and Jungfrau from swisstopo's open data (swissALTI3D 2 m elevation, SWISSIMAGE
# 2 m photos), 16 x 16 km, the newest tile of each square kilometre. The STAC listing gives every tile's SHA-256;
# penumbra-terrain (built with Penumbra) reads the listing and turns the tiles into a glTF. (c) swisstopo.
function Get-SwissAlps {
  $dir = Join-Path $root 'scenes\swiss-alps'
  $bbox = '2632,1150,2648,1166'
  $wgs = '7.84,46.49,8.09,46.655'
  $tool = Get-ChildItem (Join-Path $root 'build\*\bin\penumbra-terrain.exe') -ErrorAction SilentlyContinue | Select-Object -First 1
  if (-not $tool) { Fail 'penumbra-terrain.exe not found: build Penumbra first (.\scripts\build.ps1)' }
  $tool = $tool.FullName
  New-Item -ItemType Directory -Force (Join-Path $dir 'tiles'), (Join-Path $dir 'stac') | Out-Null
  Step 'Listing the swisstopo tiles'
  Remove-Item (Join-Path $dir 'stac\*.json') -ErrorAction SilentlyContinue
  foreach ($c in 'ch.swisstopo.swissalti3d', 'ch.swisstopo.swissimage-dop10') {
    $url = "https://data.geo.admin.ch/api/stac/v0.9/collections/$c/items?bbox=$wgs&limit=100"
    $i = 0
    while ($url) {
      $page = Join-Path $dir "stac\$c-$i.json"
      try { Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $page } catch { Fail "could not list ${url}: $_" }
      $url = (& $tool --stac-next $page | Out-String).Trim()
      $i++
    }
  }
  $pages = Get-ChildItem (Join-Path $dir 'stac\*.json') | ForEach-Object { $_.FullName }
  $list = & $tool --stac-list --bbox $bbox @pages
  if ($LASTEXITCODE -ne 0) { Fail 'the swisstopo listing is incomplete' }
  Step "Downloading $($list.Count) tiles (about 330 MB) into $dir\tiles"
  $n = 0
  foreach ($line in $list) {
    $sha, $url = $line.Trim() -split ' ', 2
    $file = Join-Path $dir ('tiles\' + ($url -split '/')[-1])
    if ((Test-Path $file) -and (Sha $file) -eq $sha) { continue }
    for ($try = 1; $try -le 3; $try++) {
      try { Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $file } catch { continue }
      if ((Sha $file) -eq $sha) { break }
    }
    if (-not (Test-Path $file) -or (Sha $file) -ne $sha) { Fail "$url could not be downloaded or does not match its SHA-256; run the script again" }
    $n++
  }
  Write-Host "    $n tile(s) downloaded"
  Step 'Building the terrain (4 m mesh, 2 m photo texture)'
  & $tool --tiles (Join-Path $dir 'tiles') --out $dir --name alps --bbox $bbox --grid 4 --texture 2 `
    --camera 2638300,1162250,2700 --target 2642835,1156511,3300 --yfov 40
  if ($LASTEXITCODE -ne 0) { Fail 'penumbra-terrain failed' }
  Write-Host 'Ready: scenes\swiss-alps.pen.json' -ForegroundColor Green
}

if ($Scene -eq 'intel-sponza') { Get-IntelSponza; $pen = 'scenes\intel-sponza.pen.json' }
elseif ($Scene -eq 'swiss-alps') { Get-SwissAlps; $pen = 'scenes\swiss-alps.pen.json' }
else { Get-Sponza; $pen = 'scenes\sponza.pen.json' }
Write-Host "Render it with: build\windows-msvc-release\bin\penumbra.exe --terminal $pen --spp 1024 -o render.png"
