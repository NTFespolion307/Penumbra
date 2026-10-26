#!/usr/bin/env bash
# Downloads the large sample scene (Sponza with textures and an HDRI sky, about 58 MB) into scenes/sponza/,
# checks every file against scenes/sponza.sha256 and adds the courtyard camera. Files already there and
# correct are kept, so running it again is quick. Render it with: penumbra --terminal scenes/sponza.pen.json
#
#   ./scripts/get-scenes.sh
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dir="$root/scenes/sponza"
manifest="$root/scenes/sponza.sha256"
# Pinned sources: the Khronos commit and the Poly Haven file never change, so the hashes stay valid.
khronos="https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/4995a638c96dbc94376d3d6164e70fee3ac7f7a2/Models/Sponza/glTF"
polyhaven="https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/2k"
keep=166936  # bytes of Sponza.gltf before its node list; the camera version replaces the rest

fail() { echo -e "\033[31mERROR: $*\033[0m" >&2; exit 1; }
step() { echo -e "\033[36m==> $*\033[0m"; }
command -v curl >/dev/null 2>&1 || fail "curl not found. Install it with: sudo apt install -y curl"
command -v sha256sum >/dev/null 2>&1 || fail "sha256sum not found (coreutils)"

ok() { [ -f "$dir/$2" ] && [ "$(sha256sum "$dir/$2" | cut -d' ' -f1)" = "$1" ]; }

mkdir -p "$dir"
step "Downloading Sponza and its sky into $dir"
count=0
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
want="$(grep ' courtyard.gltf$' "$manifest" | cut -d' ' -f1)"
ok "$want" courtyard.gltf || fail "courtyard.gltf does not match scenes/sponza.sha256"

echo -e "\033[32mReady: scenes/sponza.pen.json\033[0m"
echo "Render it with: build/linux-gcc-release/bin/penumbra --terminal scenes/sponza.pen.json --spp 1024 -o sponza.png"
