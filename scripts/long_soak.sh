#!/usr/bin/env bash

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${1:-${root_dir}/build-soak}
iterations=${2:-20}

if [[ ! "${iterations}" =~ ^[1-9][0-9]*$ ]]; then
    echo "usage: long_soak.sh [build_dir] [positive_iterations]" >&2
    exit 2
fi

echo "long_soak: ${iterations} complete unit/stress passes"
for ((iteration = 1; iteration <= iterations; ++iteration)); do
    echo "long_soak: pass ${iteration}/${iterations}"
    ctest --test-dir "${build_dir}" --output-on-failure
done
echo "long_soak: all ${iterations} passes completed"
