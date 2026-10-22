#!/usr/bin/env bash
# Runs the GPU tests (ctest label "gpu") after ./scripts/build.sh, and prints a summary to paste into a report:
# the GPUs found, then each test's result. Usage: ./scripts/test-gpu.sh [release|debug] [gpu|vulkan|cuda]
# The second argument picks the tests: all GPU tests (default), or one backend's (a GPU without ray tracing: cuda).
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
preset="linux-gcc-${1:-release}"
label="${2:-gpu}"
bin="$root/build/$preset/bin/penumbra"
if [ ! -x "$bin" ]; then
  echo "ERROR: $bin not found; run ./scripts/build.sh first" >&2
  exit 1
fi
echo "== $("$bin" --version | head -1)"
"$bin" --list-devices
echo "== GPU tests (label $label)"
cd "$root"
ctest --preset "$preset" -L "$label" --output-on-failure 2>&1 | tee "$root/build/$preset/gpu-tests.log" |
  grep -E 'Test +#|tests passed|tests failed|relMSE|Failed'
exit "${PIPESTATUS[0]}"
