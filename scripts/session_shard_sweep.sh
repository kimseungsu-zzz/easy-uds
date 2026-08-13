#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_root=${1:-/tmp/easy-uds-session-shard-sweep}
iterations=${2:-30000}
concurrency=${3:-8}
trace=${EASY_UDS_SHARD_SWEEP_TRACE:-OFF}

if [[ ! "$iterations" =~ ^[1-9][0-9]*$ || ! "$concurrency" =~ ^[1-9][0-9]*$ ]]; then
    echo "usage: session_shard_sweep.sh [build_root] [iterations] [concurrency]" >&2
    exit 2
fi

echo "Session in-flight shard sweep: iterations=$iterations concurrency=$concurrency trace=$trace"
for shards in 1 2 4 8 16; do
    build_dir="$build_root/shards-$shards"
    cmake -S "$repo_root" -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DEASY_UDS_BUILD_TESTS=OFF \
        -DEASY_UDS_BUILD_EXAMPLES=OFF \
        -DEASY_UDS_BUILD_BENCHMARKS=ON \
        -DEASY_UDS_WARNINGS_AS_ERRORS=ON \
        -DEASY_UDS_TRACE_SPIN_MISS="$trace" \
        -DEASY_UDS_TRACE_SESSION_CONTENTION="$trace" \
        -DEASY_UDS_SESSION_INFLIGHT_SHARDS="$shards" >/dev/null
    cmake --build "$build_dir" --target easy_uds_session_benchmark --parallel >/dev/null
    echo
    echo "=== shards=$shards ==="
    "$build_dir/easy_uds_session_benchmark" "$iterations" "$concurrency" shared 0
done
