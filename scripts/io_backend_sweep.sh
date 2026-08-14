#!/usr/bin/env bash

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
binary=${1:-/tmp/easy-uds-reactor-echo-ab}
repeats=${2:-3}
capability=/tmp/easy-uds-io-uring-capability

if [[ ! -x "${binary}" ]]; then
    "${CXX:-c++}" -std=c++17 -O3 -DNDEBUG \
        -Wall -Wextra -Wpedantic -Werror -pthread \
        "${root_dir}/experiments/0.6/easy_uds_io_uring_echo_probe.cpp" \
        -luring -o "${binary}"
fi

"${CXX:-c++}" -std=c++17 -O2 -DNDEBUG \
    -Wall -Wextra -Wpedantic -Werror \
    "${root_dir}/experiments/0.6/easy_uds_io_uring_probe.cpp" \
    -o "${capability}"

if ! "${capability}" | grep -q 'supported'; then
    "${capability}"
    echo "io_uring is unavailable; running the epoll reference only"
    "${binary}" 8 5000 epoll
    exit 0
fi

run_point() {
    local connections=$1
    local rounds=$2
    local repetition
    for ((repetition = 1; repetition <= repeats; ++repetition)); do
        echo "c${connections} repetition ${repetition}/${repeats}"
        "${binary}" "${connections}" "${rounds}" epoll
        "${binary}" "${connections}" "${rounds}" io_uring
    done
}

run_point 1 10000
run_point 2 10000
run_point 8 5000
run_point 32 2000

if [[ ${EASY_UDS_IO_SWEEP_STRACE:-0} == 1 ]] && command -v strace >/dev/null 2>&1; then
    echo "strace c8 epoll"
    strace -f -c "${binary}" 8 2000 epoll
    echo "strace c8 io_uring"
    strace -f -c "${binary}" 8 2000 io_uring
fi
