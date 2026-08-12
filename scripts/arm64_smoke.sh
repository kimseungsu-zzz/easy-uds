#!/usr/bin/env bash
set -euo pipefail

# Run the release/stress workload used for the 0.6.x ARM64 gate.  The script
# intentionally refuses to claim ARM coverage when invoked on another host.
if [[ "$(uname -m)" != "aarch64" && "${EASY_UDS_ALLOW_NON_ARM64:-0}" != "1" ]]; then
  echo "arm64_smoke: expected aarch64 (set EASY_UDS_ALLOW_NON_ARM64=1 for a dry build check)" >&2
  exit 2
fi

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${EASY_UDS_ARM64_BUILD_DIR:-${root_dir}/build-arm64-smoke}"

cmake -S "${root_dir}" -B "${build_dir}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DEASY_UDS_BUILD_BENCHMARKS=ON \
  -DEASY_UDS_BUILD_EXAMPLES=OFF \
  -DEASY_UDS_WARNINGS_AS_ERRORS=ON
cmake --build "${build_dir}" --parallel
ctest --test-dir "${build_dir}" --output-on-failure
"${build_dir}/easy_uds_stress_test"

echo "arm64_smoke: release build, ctest, and stress workload passed"
