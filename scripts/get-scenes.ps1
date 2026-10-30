<#
.SYNOPSIS
  Downloads the large sample scenes, which are too big for the repository, and checks every download against its
  SHA-256. Files already there and correct are kept, so running it again is quick.

.EXAMPLE
  .\scripts\get-scenes.ps1                       # Sponza (Khronos, ~58 MB) with an HDRI sky: scenes\sponza.pen.json
  .\scripts\get-scenes.ps1 -Scene intel-sponza   # Intel's New Sponza (4 GB download, 2.8 GB kept): scenes\intel-sponza.pen.json
  .\scripts\get-scenes.ps1 -Scene swiss-alps     # Eiger, Moench, Jungfrau (swisstopo, 330 MB; needs Penumbra built)
  .\scripts\get-scenes.ps1 -Scene swiss-lauterbrunnen   # The Lauterbrunnen valley (swisstopo, 330 MB)
  .\scripts\get-scenes.ps1 -Scene swiss-matterhorn      # The Matterhorn at sunrise (swisstopo + GLO-30, 340 MB)
  .\scripts\get-scenes.ps1 -Scene swiss-rigi            # A sea of fog from Rigi (swisstopo + GLO-30, 770 MB)
#>
[CmdletBinding()]
param(
  [ValidateSet('sponza', 'intel-sponza', 'swiss-alps', 'swiss-lauterbrunnen', 'swiss-matterhorn', 'swiss-rigi')] [string] $Scene = 'sponza'
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

# Swiss terrain (M14): real mountains from swisstopo's open data (swissALTI3D 2 m elevation, SWISSIMAGE 2 m photos),
# the newest tile of each square kilometre. The STAC listing gives every tile's SHA-256; penumbra-terrain (built with
# Penumbra) reads the listing and turns the tiles into a glTF. (c) swisstopo.
# $bbox: LV95 kilometres; $wgs: the same area in WGS84 degrees (for the listing); camera/target: LV95 metres.
# With $farBox (LV95 km) and $glo (GLO-30 tile names, e.g. 'N46_00_E007_00 N47_00_E007_00'), Copernicus GLO-30 tiles
# (scenes\copernicus.sha256) add the distant mountains around the area and fill the parts outside Switzerland.
function Get-Swiss([string] $name, [string] $base, [string] $bbox, [string] $wgs, [string] $size, [string] $camera,
                   [string] $target, [string] $yfov, [string] $farBox = '', [string] $glo = '') {
  $dir = Join-Path $root "scenes\$name"
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
  $partial = @()
  $far = @()
  if ($farBox) {
    $gdir = Join-Path $root 'scenes\copernicus'
    New-Item -ItemType Directory -Force $gdir | Out-Null
    Step "Downloading the far-field elevation (Copernicus GLO-30) into $gdir"
    $manifest = Get-Content (Join-Path $root 'scenes\copernicus.sha256')
    foreach ($t in $glo -split ' ') {
      $f = "Copernicus_DSM_COG_10_${t}_DEM"
      $want = ($manifest | Where-Object { $_ -match " $f$" } | Select-Object -First 1) -split ' ' | Select-Object -First 1
      if (-not $want) { Fail "$f is not in scenes\copernicus.sha256" }
      $file = Join-Path $gdir "$f.tif"
      if ((Test-Path $file) -and (Sha $file) -eq $want) { continue }
      try { Invoke-WebRequest -UseBasicParsing -Uri "https://copernicus-dem-30m.s3.amazonaws.com/$f/$f.tif" -OutFile $file }
      catch { Fail "could not download ${f}: $_" }
      if ((Sha $file) -ne $want) { Fail "$f.tif does not match scenes\copernicus.sha256 (damaged download?); run the script again" }
    }
    $partial = @('--partial')
    $far = @('--far', $gdir, '--far-bbox', $farBox)
  }
  $pages = Get-ChildItem (Join-Path $dir 'stac\*.json') | ForEach-Object { $_.FullName }
  $list = & $tool --stac-list --bbox $bbox @partial @pages
  if ($LASTEXITCODE -ne 0) { Fail 'the swisstopo listing is incomplete' }
  Step "Downloading $($list.Count) tiles (about $size) into $dir\tiles"
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
  & $tool --tiles (Join-Path $dir 'tiles') --out $dir --name $base --bbox $bbox --grid 4 --texture 2 `
    --camera $camera --target $target --yfov $yfov @far
  if ($LASTEXITCODE -ne 0) { Fail 'penumbra-terrain failed' }
  Write-Host "Ready: scenes\$name.pen.json" -ForegroundColor Green
}

if ($Scene -eq 'intel-sponza') { Get-IntelSponza; $pen = 'scenes\intel-sponza.pen.json' }
elseif ($Scene -eq 'swiss-alps') {  # Eiger, Moench and Jungfrau, 16 x 16 km
  Get-Swiss swiss-alps alps '2632,1150,2648,1166' '7.84,46.49,8.09,46.655' '330 MB' `
    '2638300,1162250,2700' '2642835,1156511,3300' 40
  $pen = 'scenes\swiss-alps.pen.json'
}
elseif ($Scene -eq 'swiss-lauterbrunnen') {  # the Lauterbrunnen valley from the north, 16 x 16 km
  Get-Swiss swiss-lauterbrunnen lauterbrunnen '2628,1148,2644,1164' '7.78,46.47,8.03,46.64' '330 MB' `
    '2636250,1162900,1500' '2635300,1152000,1100' 45
  $pen = 'scenes\swiss-lauterbrunnen.pen.json'
}
elseif ($Scene -eq 'swiss-matterhorn') {  # the Matterhorn from the Gornergrat, 16 x 16 km; the Italian side from GLO-30
  Get-Swiss swiss-matterhorn matterhorn '2612,1084,2628,1100' '7.57,45.89,7.82,46.07' '340 MB' `
    '2626800,1092500,3140' '2617050,1091700,3900' 30 '2612,1084,2628,1100' 'N45_00_E007_00 N46_00_E007_00'
  $pen = 'scenes\swiss-matterhorn.pen.json'
}
elseif ($Scene -eq 'swiss-rigi') {  # a sea of fog from Rigi Kulm: 20 x 20 km at 2 m, GLO-30 out to the Bernese Alps
  Get-Swiss swiss-rigi rigi '2670,1198,2690,1218' '8.34,46.92,8.64,47.12' '770 MB' `
    '2679500,1212200,1820' '2672000,1160000,1300' 40 '2610,1125,2730,1235' `
    'N46_00_E007_00 N46_00_E008_00 N46_00_E009_00 N47_00_E007_00 N47_00_E008_00 N47_00_E009_00'
  $pen = 'scenes\swiss-rigi.pen.json'
}
else { Get-Sponza; $pen = 'scenes\sponza.pen.json' }
Write-Host "Render it with: build\windows-msvc-release\bin\penumbra.exe --terminal $pen --spp 1024 -o render.png"
