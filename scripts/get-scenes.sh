#!/usr/bin/env bash
# Downloads the large sample scenes, which are too big for the repository, and checks every download against its
# SHA-256. Files already there and correct are kept, so running it again is quick.
#
#   ./scripts/get-scenes.sh                 Sponza (Khronos, ~58 MB) with an HDRI sky: scenes/sponza.pen.json
#   ./scripts/get-scenes.sh intel-sponza    Intel's New Sponza (4 GB download, 2.8 GB kept): scenes/intel-sponza.pen.json
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

case "$scene" in
  sponza) get_sponza; exe="scenes/sponza.pen.json" ;;
  intel-sponza) get_intel_sponza; exe="scenes/intel-sponza.pen.json" ;;
  -h|--help) sed -n '2,7p' "$0"; exit 0 ;;
  *) fail "unknown scene '$scene' (sponza, intel-sponza)" ;;
esac
echo "Render it with: build/linux-gcc-release/bin/penumbra --terminal $exe --spp 1024 -o render.png"
