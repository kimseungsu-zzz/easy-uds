#!/usr/bin/env bash
set -euo pipefail

binary=${1:-build-bench/easy_uds_session_benchmark}
iterations=${2:-20000}

if [[ ! -x "$binary" ]]; then
    echo "benchmark binary not found or not executable: $binary" >&2
    exit 2
fi

echo "shared Session contention sweep: binary=$binary iterations=$iterations"
for concurrency in 1 2 4 8 16 32; do
    echo
    echo "=== concurrency=$concurrency ==="
    "$binary" "$iterations" "$concurrency" shared 0
done
