#!/usr/bin/env bash

set -euo pipefail

build_dir=${1:?usage: simple_core_benchmark.sh <build-dir> [iterations]}
iterations=${2:-30000}

for concurrency in 1 8 32; do
    for api in core simple; do
        binary="${build_dir}/easy_uds_${api}_api_benchmark"
        if [[ ! -x "${binary}" ]]; then
            echo "missing benchmark binary: ${binary}" >&2
            exit 2
        fi
        echo "=== ${api} c${concurrency} ==="
        if [[ -x /usr/bin/time ]]; then
            /usr/bin/time -f \
                'process: max_rss=%M KiB, vcs=%w, ivcs=%c' \
                "${binary}" "${iterations}" "${concurrency}"
        else
            "${binary}" "${iterations}" "${concurrency}"
        fi
    done
done

echo "=== allocation comparison ==="
"${build_dir}/easy_uds_core_api_allocation_benchmark" "${iterations}"
"${build_dir}/easy_uds_simple_api_allocation_benchmark" "${iterations}"
