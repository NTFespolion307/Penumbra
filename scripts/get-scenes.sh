#!/usr/bin/env bash
# Downloads the large sample scenes, which are too big for the repository, and checks every download against its
# SHA-256. Files already there and correct are kept, so running it again is quick.
#
#   ./scripts/get-scenes.sh                 Sponza (Khronos, ~58 MB) with an HDRI sky: scenes/sponza.pen.json
#   ./scripts/get-scenes.sh intel-sponza    Intel's New Sponza (4 GB download, 2.8 GB kept): scenes/intel-sponza.pen.json
#   ./scripts/get-scenes.sh swiss-alps      Eiger, Moench, Jungfrau (swisstopo, 330 MB; needs Penumbra built): scenes/swiss-alps.pen.json
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
scene="${1:-sponza}"

fail() { echo -e "\033[31mERROR: $*\033[0m" >&2; exit 1; }
step() { echo -e "\033[36m==> $*\033[0m"; }
command -v curl >/dev/null 2>&1 || fail "curl not found. Install it with: sudo apt install -y curl"
command -v sha256sum >/dev/null 2>&1 || fail "sha256sum not found (coreutils)"

# The Khronos Sponza: every file pinned to one commit, the sky from Poly Haven, and a courtyard camera added.
get_sponza() {
  local dir="$root/scenes/sponza" manifest="$root/scenes/sponza.sha256"
  local khronos="https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/4995a638c96dbc94376d3d6164e70fee3ac7f7a2/Models/Sponza/glTF"
  local polyhaven="https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/2k"
  local keep=166936  # bytes of Sponza.gltf before its node list; the camera version replaces the rest
  ok() { [ -f "$dir/$2" ] && [ "$(sha256sum "$dir/$2" | cut -d' ' -f1)" = "$1" ]; }

  mkdir -p "$dir"
  step "Downloading Sponza and its sky into $dir"
  local count=0 hash name url
  while read -r hash name; do
    case "$hash" in ''|'#'*) continue ;; esac
    [ "$name" = courtyard.gltf ] && continue
    ok "$hash" "$name" && continue
    case "$name" in *.hdr) url="$polyhaven/$name" ;; *) url="$khronos/$name" ;; esac
    curl -fsSL --retry 3 -o "$dir/$name" "$url" || fail "could not download $url"
    ok "$hash" "$name" || fail "$name does not match scenes/sponza.sha256 (damaged download?); run the script again"
    count=$((count + 1))
  done < "$manifest"
  echo "    $count file(s) downloaded"

  step "Adding the courtyard camera"
  { head -c "$keep" "$dir/Sponza.gltf"; cat "$root/scenes/sponza-camera.txt"; } > "$dir/courtyard.gltf"
  local want
  want="$(grep ' courtyard.gltf$' "$manifest" | cut -d' ' -f1)"
  ok "$want" courtyard.gltf || fail "courtyard.gltf does not match scenes/sponza.sha256"
  echo -e "\033[32mReady: scenes/sponza.pen.json\033[0m"
}

# Intel's New Sponza (main scene): one 4 GB zip from Intel, checked, then only the glTF, its buffer, the textures
# and the HDRI are kept (the 3ds Max, FBX and USD versions are left out). CC BY 4.0, see the README.
get_intel_sponza() {
  local dir="$root/scenes/intel-sponza" url="https://cdrdv2.intel.com/v1/dl/getContent/830833"
  local want="b8bb853884ab1566b3beb35666bd09882a4e0dc16661e4684e103792cf0229b9"
  local zip="$dir/main_sponza.zip" marker="$dir/.extracted-$want"
  command -v unzip >/dev/null 2>&1 || fail "unzip not found. Install it with: sudo apt install -y unzip"
  mkdir -p "$dir"
  if [ -f "$marker" ]; then
    echo -e "\033[32mReady (already there): scenes/intel-sponza.pen.json\033[0m"
    return
  fi
  if [ ! -f "$zip" ] || [ "$(sha256sum "$zip" | cut -d' ' -f1)" != "$want" ]; then
    step "Downloading Intel's New Sponza (4 GB) into $dir"
    curl -fL --retry 5 -o "$zip" "$url" || fail "could not download $url"
  fi
  step "Checking the download"
  [ "$(sha256sum "$zip" | cut -d' ' -f1)" = "$want" ] || fail "main_sponza.zip does not match its SHA-256 (damaged or changed download); delete it and run the script again"
  step "Extracting the glTF version"
  unzip -oq "$zip" 'main_sponza/NewSponza_Main_glTF_003.gltf' 'main_sponza/NewSponza_Main_glTF_003.bin' \
    'main_sponza/textures/*' 'main_sponza/credits_license.txt' -x 'main_sponza/textures/desktop.ini' -d "$dir"
  rm -f "$zip"
  touch "$marker"
  echo -e "\033[32mReady: scenes/intel-sponza.pen.json\033[0m"
}

# The Swiss Alps (M14): Eiger, Moench and Jungfrau from swisstopo's open data (swissALTI3D 2 m elevation, SWISSIMAGE
# 2 m photos), 16 x 16 km, the newest tile of each square kilometre. The STAC listing gives every tile's SHA-256;
# penumbra-terrain (built with Penumbra) reads the listing and turns the tiles into a glTF. (c) swisstopo.
get_swiss_alps() {
  local dir="$root/scenes/swiss-alps" bbox=2632,1150,2648,1166 wgs=7.84,46.49,8.09,46.655
  local stac="https://data.geo.admin.ch/api/stac/v0.9/collections" tool c url i
  tool="$(ls "$root"/penumbra-terrain "$root"/build/*/bin/penumbra-terrain 2>/dev/null | head -1 || true)"  # release or build
  [ -n "$tool" ] || fail "penumbra-terrain not found: build Penumbra first (./scripts/build.sh)"
  mkdir -p "$dir/tiles" "$dir/stac"
  step "Listing the swisstopo tiles"
  rm -f "$dir"/stac/*.json
  for c in ch.swisstopo.swissalti3d ch.swisstopo.swissimage-dop10; do
    url="$stac/$c/items?bbox=$wgs&limit=100"
    i=0
    while [ -n "$url" ]; do
      curl -fsSL --retry 3 -o "$dir/stac/$c-$i.json" "$url" || fail "could not list $url"
      url="$("$tool" --stac-next "$dir/stac/$c-$i.json")"
      i=$((i + 1))
    done
  done
  "$tool" --stac-list --bbox "$bbox" "$dir"/stac/*.json > "$dir/tiles.txt" || fail "the swisstopo listing is incomplete"
  step "Downloading $(wc -l < "$dir/tiles.txt") tiles (about 330 MB) into $dir/tiles"
  get_tile() {  # sha256 url dir: keeps a correct file, else downloads and checks it
    local f="$3/tiles/${2##*/}" try
    [ -f "$f" ] && [ "$(sha256sum "$f" | cut -d' ' -f1)" = "$1" ] && return 0
    for try in 1 2 3; do
      curl -fsSL --retry 3 -o "$f" "$2" && [ "$(sha256sum "$f" | cut -d' ' -f1)" = "$1" ] && return 0
    done
    echo "could not download $2 (or it does not match its SHA-256)" >&2
    return 1
  }
  export -f get_tile
  xargs -P 8 -L 1 bash -c 'get_tile "$0" "$1" "$2"' < <(sed "s|\$| $dir|" "$dir/tiles.txt") ||
    fail "some tiles failed; run the script again"
  step "Building the terrain (4 m mesh, 2 m photo texture)"
  "$tool" --tiles "$dir/tiles" --out "$dir" --name alps --bbox "$bbox" --grid 4 --texture 2 \
    --camera 2638300,1162250,2700 --target 2642835,1156511,3300 --yfov 40 || fail "penumbra-terrain failed"
  echo -e "\033[32mReady: scenes/swiss-alps.pen.json\033[0m"
}

case "$scene" in
  sponza) get_sponza; exe="scenes/sponza.pen.json" ;;
  intel-sponza) get_intel_sponza; exe="scenes/intel-sponza.pen.json" ;;
  swiss-alps) get_swiss_alps; exe="scenes/swiss-alps.pen.json" ;;
  -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
  *) fail "unknown scene '$scene' (sponza, intel-sponza, swiss-alps)" ;;
esac
echo "Render it with: build/linux-gcc-release/bin/penumbra --terminal $exe --spp 1024 -o render.png"
