#!/usr/bin/env bash

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${1:-${root_dir}/build-soak}
iterations=${2:-20}
run_benchmarks=${EASY_UDS_SOAK_BENCHMARKS:-0}

if [[ ! "${iterations}" =~ ^[1-9][0-9]*$ ]]; then
    echo "usage: long_soak.sh [build_dir] [positive_iterations]" >&2
    exit 2
fi
if [[ "${run_benchmarks}" != "0" && "${run_benchmarks}" != "1" ]]; then
    echo "EASY_UDS_SOAK_BENCHMARKS must be 0 or 1" >&2
    exit 2
fi
test_count=$(ctest --test-dir "${build_dir}" -N 2>/dev/null |
    awk '/Total Tests:/{print $3; exit}')
if [[ -z "${test_count}" || "${test_count}" -eq 0 ]]; then
    echo "long_soak: build directory contains no CTest tests" >&2
    exit 2
fi

run_soak_benchmarks() {
    if [[ "${run_benchmarks}" != "1" ]]; then
        return 0
    fi
    if [[ ! -x "${build_dir}/easy_uds_rpc_benchmark" ||
          ! -x "${build_dir}/easy_uds_session_benchmark" ||
          ! -x "${build_dir}/easy_uds_stream_benchmark" ]]; then
        echo "long_soak: benchmark targets are unavailable; skipping optional workload pass"
        return 0
    fi
    echo "long_soak: optional workload pass (one-shot/session/stream)"
    run_workload() {
        local label=$1
        shift
        local pid
        local max_fd=0
        local max_threads=0
        if [[ -x /usr/bin/time ]]; then
            /usr/bin/time -f \
                'soak process: user=%U s, system=%S s, max_rss=%M KiB, vcs=%w, ivcs=%c' \
                "$@" &
        else
            "$@" &
        fi
        pid=$!
        while kill -0 "${pid}" 2>/dev/null; do
            if [[ -d "/proc/${pid}/fd" ]]; then
                local fd_count
                fd_count=$(find "/proc/${pid}/fd" -mindepth 1 -maxdepth 1 -type l 2>/dev/null | wc -l)
                if (( fd_count > max_fd )); then
                    max_fd=${fd_count}
                fi
            fi
            if [[ -d "/proc/${pid}/task" ]]; then
                local thread_count
                thread_count=$(find "/proc/${pid}/task" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | wc -l)
                if (( thread_count > max_threads )); then
                    max_threads=${thread_count}
                fi
            fi
            sleep 0.02
        done
        wait "${pid}"
        echo "soak workload=${label}: max_fd=${max_fd}, max_threads=${max_threads}"
    }

    run_workload one-shot "${build_dir}/easy_uds_rpc_benchmark" 2000 1 0
    run_workload shared-session "${build_dir}/easy_uds_session_benchmark" 3000 4 shared 1
    run_workload streaming "${build_dir}/easy_uds_stream_benchmark" 16 65536
}

echo "long_soak: ${iterations} complete unit/stress passes"
for ((iteration = 1; iteration <= iterations; ++iteration)); do
    echo "long_soak: pass ${iteration}/${iterations}"
    ctest --test-dir "${build_dir}" --output-on-failure
    run_soak_benchmarks
done
echo "long_soak: all ${iterations} passes completed"
