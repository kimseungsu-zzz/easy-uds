#!/usr/bin/env bash

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
binary=${1:-/tmp/easy-uds-shm-process-probe}

if [[ ! -x "${binary}" ]]; then
    "${CXX:-c++}" -std=c++17 -O3 -DNDEBUG \
        -Wall -Wextra -Wpedantic -Werror \
        -I"${root_dir}/include" -I"${root_dir}/src" -pthread \
        "${root_dir}/tests/easy_uds_shm_process_probe.cpp" \
        -o "${binary}"
fi

for spin in 0 64 256 1024 2048; do
    echo "hot spin=${spin}"
    "${binary}" 64 5000 0 "${spin}"
    echo "idle spin=${spin}"
    "${binary}" 4096 30 2000 "${spin}"
done
