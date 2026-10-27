#!/usr/bin/env bash
# Builds Penumbra on Linux: checks prerequisites, configures, builds, runs the tests and a smoke test,
# and prints where the binary is.
#
#   ./scripts/build.sh             Release build with tests
#   ./scripts/build.sh --debug     Debug build
#   ./scripts/build.sh --no-tests --clean
#   ./scripts/build.sh --quick          Only the quick tests (unit, smoke, lint; under a minute)
#   ./scripts/build.sh --ui | --no-ui    Force the user interface on or off (default: on when a display is present)
set -euo pipefail

config=release
tests=1
quick=0
clean=0
ui=auto
for arg in "$@"; do
  case "$arg" in
    --debug) config=debug ;;
    --release) config=release ;;
    --no-tests) tests=0 ;;
    --quick) quick=1 ;;
    --clean) clean=1 ;;
    --ui) ui=1 ;;
    --no-ui) ui=0 ;;
    -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
    *) echo "unknown option: $arg" >&2; exit 1 ;;
  esac
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
preset="linux-gcc-$config"

fail() { echo -e "\033[31mERROR: $*\033[0m" >&2; exit 1; }
step() { echo -e "\033[36m==> $*\033[0m"; }
need() { command -v "$1" >/dev/null 2>&1 || fail "$1 not found. $2"; }

step "Checking prerequisites"
need git "Install it with: sudo apt install -y git"
need gcc-12 "Install it with: sudo apt install -y gcc-12 g++-12"
need g++-12 "Install it with: sudo apt install -y gcc-12 g++-12"
need ninja "Install it with: sudo apt install -y ninja-build"
for tool in curl zip unzip tar pkg-config; do
  need "$tool" "Install it with: sudo apt install -y curl zip unzip tar pkg-config"
done
need cmake "Install CMake 3.26+ as shown in README.md \"Prerequisites\""

cmake_version="$(cmake --version | head -n1 | grep -oE '[0-9]+\.[0-9]+' | head -n1)"
cmake_major="${cmake_version%%.*}"
cmake_minor="${cmake_version#*.}"
if (( cmake_major < 3 || (cmake_major == 3 && cmake_minor < 26) )); then
  fail "CMake 3.26 or newer is required, found $cmake_version (Ubuntu 22.04 ships 3.22; see README.md)"
fi
echo "    CMake: $cmake_version ($(command -v cmake))"
echo "    GCC:   $(g++-12 -dumpfullversion)"

if [[ -z "${VCPKG_ROOT:-}" || ! -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]]; then
  fail "vcpkg not found. Install it (README.md \"Prerequisites\") and set VCPKG_ROOT, for example ~/vcpkg"
fi
echo "    vcpkg: $VCPKG_ROOT"

# Parallel jobs: one per core, but at most one per 2 GB of available RAM. Embree's kernels need about 2 GB per
# GCC process; running out of memory gets a compiler killed and vcpkg falls back to a single core.
# nproc follows the affinity mask but not a container's CPU quota (cgroup v2 cpu.max): a container can show 192
# cores and be allowed 23 (DECISIONS D-081).
cores="$(nproc)"
quota="max" period=0
if [[ -r /sys/fs/cgroup/cpu.max ]]; then                       # cgroup v2
  read -r quota period < /sys/fs/cgroup/cpu.max
elif [[ -r /sys/fs/cgroup/cpu/cpu.cfs_quota_us ]]; then         # cgroup v1 (-1 = no limit)
  quota="$(cat /sys/fs/cgroup/cpu/cpu.cfs_quota_us)"
  period="$(cat /sys/fs/cgroup/cpu/cpu.cfs_period_us)"
  (( quota < 0 )) && quota="max"
fi
if [[ "$quota" != "max" && "$period" -gt 0 ]]; then
  allowed=$(( (quota + period - 1) / period ))
  (( allowed < cores )) && cores=$allowed
fi
if [[ -z "${VCPKG_MAX_CONCURRENCY:-}" ]]; then
  mem_gb="$(awk '/MemAvailable/ {print int($2 / 1048576)}' /proc/meminfo)"
  jobs=$(( mem_gb / 2 ))
  (( jobs < 1 )) && jobs=1
  (( jobs > cores )) && jobs=$cores
  export VCPKG_MAX_CONCURRENCY=$jobs
  echo "    parallel jobs: $jobs ($cores cores, ${mem_gb} GB RAM available, ~2 GB per job; set VCPKG_MAX_CONCURRENCY to override)"
else
  echo "    parallel jobs: $VCPKG_MAX_CONCURRENCY (from VCPKG_MAX_CONCURRENCY)"
fi
export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-$VCPKG_MAX_CONCURRENCY}"

cd "$root"
if (( clean )) && [[ -d "build/$preset" ]]; then
  step "Removing build/$preset"
  rm -rf "build/$preset"
fi

# The UI needs a display; servers and rented containers build without it (DECISIONS D-100). Its window library
# needs the X11 development headers.
if [[ "$ui" == auto ]]; then
  if [[ -n "${DISPLAY:-}" || -n "${WAYLAND_DISPLAY:-}" ]]; then ui=1; else ui=0; fi
  if (( ui )); then step "User interface: on (a display is present; --no-ui to skip)"
  else step "User interface: off (no display found; --ui to build it anyway)"; fi
fi
if (( ui )); then
  pkg-config --exists x11 xrandr xinerama xcursor xi 2>/dev/null ||
    fail "the user interface needs the X11 development packages. Install them with:
  sudo apt install -y libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev
or build without the UI: ./scripts/build.sh --no-ui"
fi

step "Configuring ($preset); the first run builds the dependencies and takes a few minutes"
cmake --preset "$preset" -DPEN_ENABLE_UI=$( (( ui )) && echo ON || echo OFF )

step "Building"
cmake --build --preset "$preset"

if (( tests )); then
  step "Running tests"
  # Side by side: ctest counts each test's PROCESSORS against the allowed cores (D-106). 20K gate: ctest -L large;
  # GPU: scripts/test-gpu.sh.
  if (( quick )); then
    ctest --preset "$preset" -L "^(unit|smoke|lint)$" -j "$cores"
  else
    ctest --preset "$preset" -LE "large|gpu" -j "$cores"
  fi
fi

exe="$root/build/$preset/bin/penumbra"
step "Smoke test"
"$exe" --version

echo
echo -e "\033[32mBuilt: $exe\033[0m"
