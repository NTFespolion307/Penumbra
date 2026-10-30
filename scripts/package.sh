#!/usr/bin/env bash
# Builds the Linux release: dist/penumbra-linux-x64.tar.gz (+ .sha256), a headless penumbra for servers and
# workers that runs without installing anything but a GPU driver (docs/DECISIONS.md D-129).
#
#   ./scripts/package.sh
#
# Same prerequisites as scripts/build.sh, plus the CUDA toolkit for the CUDA backend (without it the release has
# Vulkan and the CPU only, and says so). Build on the oldest supported system (Ubuntu 22.04): the binary needs
# glibc 2.35 or newer. libstdc++ is linked in (PEN_PORTABLE); Open Image Denoise and TBB travel next to it.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$root/build/linux-package"
stage="$root/dist/penumbra"
out="$root/dist/penumbra-linux-x64.tar.gz"
fail() { echo -e "\033[31mERROR: $*\033[0m" >&2; exit 1; }
step() { echo -e "\033[36m==> $*\033[0m"; }

[[ -n "${VCPKG_ROOT:-}" && -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]] ||
  fail "VCPKG_ROOT is not set to a vcpkg checkout (see README.md \"Prerequisites\")"
export PATH="/usr/local/cuda/bin:$PATH"
if command -v nvcc >/dev/null 2>&1; then cuda=ON; else cuda=OFF; echo "    no CUDA toolkit: the release has Vulkan and the CPU only"; fi

step "Configuring a headless release build in $build"
cmake --preset linux-gcc-release -B "$build" -DPEN_ENABLE_UI=OFF -DPEN_BUILD_TESTS=OFF -DPEN_PORTABLE=ON \
  -DPEN_ENABLE_CUDA=$cuda
step "Building"
cmake --build "$build"

bin="$build/bin"
step "Smoke test"
"$bin/penumbra" --version
"$bin/penumbra" --terminal "$root/scenes/cornell.pen.json" --res 64x64 --spp 4 --backend cpu -o "$build/smoke.png" \
  > "$build/smoke.log" 2>&1 || { cat "$build/smoke.log"; fail "the smoke render failed"; }

step "Staging $stage"
rm -rf "$stage" && mkdir -p "$stage/scripts" "$stage/scenes" "$stage/licenses"
cp "$bin/penumbra" "$bin/penumbra-terrain" "$stage/"
cp -P "$bin"/*.so* "$stage/"                     # Open Image Denoise and TBB (penumbra looks in its own folder)
cp "$root/scripts/get-scenes.sh" "$stage/scripts/"
cp "$root"/scenes/*.pen.json "$root"/scenes/*.gltf "$root"/scenes/*.png "$root"/scenes/*.hdr "$root"/scenes/sponza.sha256 \
  "$root"/scenes/sponza-camera.txt "$stage/scenes/"
cp "$root/LICENSE" "$stage/licenses/Penumbra-LICENSE.txt"
oidn="$(ls -d "$build"/_deps/oidn-*/doc)"
cp "$oidn/LICENSE.txt" "$stage/licenses/OpenImageDenoise-LICENSE.txt"
cp "$oidn"/third-party-programs*.txt "$stage/licenses/"
version="$("$bin/penumbra" --version | head -n1)"
cat > "$stage/README.txt" <<EOF
$version
Linux x64 release (headless: --terminal and --worker). Needs glibc 2.35+ (Ubuntu 22.04+, Debian 12+) and a GPU
driver: NVIDIA 580+ for CUDA 13 and Vulkan ray tracing, or any Vulkan driver with ray tracing; else the CPU renders.

  ./penumbra --list-devices
  ./penumbra --terminal scenes/cornell.pen.json --spp 256 -o cornell.png
  ./penumbra --worker --join <controller address> --code <join code> --stay

Every machine of a render must run the same version: download this release on all of them.
Sample scenes: ./scripts/get-scenes.sh (Sponza), ./scripts/get-scenes.sh swiss-alps.
Full documentation: https://github.com/NTFespolion307/Penumbra#readme
EOF

step "Checking that it needs nothing beyond the system and its own folder"
missing="$(cd "$stage" && LD_LIBRARY_PATH= ldd ./penumbra | grep 'not found' || true)"
[[ -z "$missing" ]] || fail "missing libraries: $missing"
ldd "$stage/penumbra" | grep -vE 'linux-vdso|ld-linux|libc\.so|libm\.so|libdl\.so|libpthread\.so|librt\.so|\./|dist/penumbra' || true

step "Writing $out"
tar -C "$root/dist" -czf "$out" penumbra
(cd "$root/dist" && sha256sum "$(basename "$out")" > "$(basename "$out").sha256")
echo -e "\033[32mBuilt: $out ($(du -h "$out" | cut -f1)), $(cut -d' ' -f1 "$out.sha256")\033[0m"
