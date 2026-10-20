#!/usr/bin/env bash
# Runs the GPU tests (ctest label "gpu") after ./scripts/build.sh, and prints a summary to paste into a report:
# the GPUs found, then each test's result. Usage: ./scripts/test-gpu.sh [release|debug]
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
preset="linux-gcc-${1:-release}"
bin="$root/build/$preset/bin/penumbra"
if [ ! -x "$bin" ]; then
  echo "ERROR: $bin not found; run ./scripts/build.sh first" >&2
  exit 1
fi
echo "== $("$bin" --version | head -1)"
"$bin" --list-devices
echo "== GPU tests"
cd "$root"
ctest --preset "$preset" -L gpu --output-on-failure 2>&1 | tee "$root/build/$preset/gpu-tests.log" |
  grep -E 'Test +#|tests passed|tests failed|relMSE|Failed'
exit "${PIPESTATUS[0]}"
