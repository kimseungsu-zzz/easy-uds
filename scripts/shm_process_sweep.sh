#!/usr/bin/env bash

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
binary=${1:-/tmp/easy-uds-shm-process-probe}

if [[ ! -x "${binary}" ]]; then
    "${CXX:-c++}" -std=c++17 -O3 -DNDEBUG \
        -Wall -Wextra -Wpedantic -Werror \
        -I"${root_dir}/include" -I"${root_dir}/src" -pthread \
        "${root_dir}/experiments/0.6/easy_uds_shm_process_probe.cpp" \
        -o "${binary}"
fi

run_point() {
    local payload=$1
    local rounds=$2
    "${binary}" "${payload}" "${rounds}"
}

run_point 64 20000
run_point 256 15000
run_point 1024 10000
run_point 4096 5000
run_point 16384 2000
run_point 65536 500
run_point 262144 200
run_point 1048576 50
run_point 4194304 10
