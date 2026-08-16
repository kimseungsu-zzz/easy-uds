#!/usr/bin/env bash

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${1:-${root_dir}/build-final-benchmarks}

require_binary() {
    if [[ ! -x "${build_dir}/$1" ]]; then
        echo "final_linux_benchmarks: missing ${build_dir}/$1" >&2
        exit 2
    fi
}

run_timed() {
    local label=$1
    shift
    echo
    echo "=== ${label} ==="
    if [[ -x /usr/bin/time ]]; then
        /usr/bin/time -f \
            'process: user=%U s, system=%S s, cpu=%P, max_rss=%M KiB, vcs=%w, ivcs=%c' \
            "$@"
    else
        "$@"
    fi
}

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "final_linux_benchmarks: Linux is required" >&2
    exit 2
fi

require_binary easy_uds_rpc_benchmark
require_binary easy_uds_session_benchmark
require_binary easy_uds_stream_benchmark
require_binary easy_uds_allocation_benchmark

echo "easy-uds 0.8 Linux benchmark"
echo "utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "kernel: $(uname -srvmo)"
echo "architecture: $(uname -m)"
echo "compiler: $(${CXX:-c++} --version | head -n 1)"
echo "cmake: $(cmake --version | head -n 1)"
if command -v lscpu >/dev/null 2>&1; then
    lscpu | grep -E '^(Architecture|CPU\(s\)|Model name|Thread|Core|Socket|CPU max MHz|CPU min MHz):' || true
fi

run_timed "one-shot RPC c1, empty payload" \
    "${build_dir}/easy_uds_rpc_benchmark" 20000 1 0
run_timed "one-shot RPC c8, empty payload" \
    "${build_dir}/easy_uds_rpc_benchmark" 40000 8 0
run_timed "one-shot RPC c1, 1 KiB payload" \
    "${build_dir}/easy_uds_rpc_benchmark" 10000 1 1024
run_timed "one-shot RPC c1, 64 KiB payload" \
    "${build_dir}/easy_uds_rpc_benchmark" 2000 1 65536
run_timed "one-shot RPC c1, 1 MiB payload" \
    "${build_dir}/easy_uds_rpc_benchmark" 200 1 1048576

run_timed "persistent independent sessions c8" \
    "${build_dir}/easy_uds_session_benchmark" 50000 8
run_timed "one shared session c1" \
    "${build_dir}/easy_uds_session_benchmark" 30000 1 shared 1
run_timed "one shared session c8" \
    "${build_dir}/easy_uds_session_benchmark" 50000 8 shared 1
run_timed "one shared session c32" \
    "${build_dir}/easy_uds_session_benchmark" 64000 32 shared 1
run_timed "one shared session c64" \
    "${build_dir}/easy_uds_session_benchmark" 64000 64 shared 1

run_timed "streaming, 64 KiB chunks" \
    "${build_dir}/easy_uds_stream_benchmark" 256 65536

run_timed "allocation, warm session" \
    "${build_dir}/easy_uds_allocation_benchmark" 20000
run_timed "allocation, serialized executor" \
    "${build_dir}/easy_uds_allocation_benchmark" 5000 serialized
run_timed "allocation, named serialized domain" \
    "${build_dir}/easy_uds_allocation_benchmark" 5000 domain
run_timed "allocation, 1 MiB stream" \
    "${build_dir}/easy_uds_allocation_benchmark" 50 stream 1048576

if [[ -x "${build_dir}/easy_uds_shm_process_probe" ]]; then
    run_timed "process SHM A/B, 4 KiB" \
        "${build_dir}/easy_uds_shm_process_probe" 4096 2000
    run_timed "process SHM A/B, 1 MiB" \
        "${build_dir}/easy_uds_shm_process_probe" 1048576 50
fi

if [[ -x "${build_dir}/easy_uds_io_uring_probe" &&
      -x "${build_dir}/easy_uds_io_uring_echo_probe" ]] &&
   "${build_dir}/easy_uds_io_uring_probe" | grep -q 'supported'; then
    run_timed "epoll/io_uring echo A/B c2" \
        "${build_dir}/easy_uds_io_uring_echo_probe" 2 10000 both
    run_timed "epoll/io_uring echo A/B c8" \
        "${build_dir}/easy_uds_io_uring_echo_probe" 8 5000 both
    run_timed "epoll/io_uring echo A/B c32" \
        "${build_dir}/easy_uds_io_uring_echo_probe" 32 2000 both
else
    echo
    echo "=== epoll/io_uring echo A/B ==="
    echo "io_uring unavailable; A/B skipped"
fi

echo
echo "final_linux_benchmarks: completed"
