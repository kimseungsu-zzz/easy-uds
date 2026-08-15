#!/usr/bin/env bash

set -euo pipefail

build_dir=${1:?usage: simple_core_benchmark_median.sh <build-dir> [iterations] [repeats]}
iterations=${2:-10000}
repeats=${3:-10}

if ! [[ "${iterations}" =~ ^[1-9][0-9]*$ && "${repeats}" =~ ^[1-9][0-9]*$ ]]; then
    echo "iterations and repeats must be positive integers" >&2
    exit 2
fi
if (( repeats < 10 )); then
    echo "performance decisions require at least 10 alternating repeats" >&2
    exit 2
fi

tmp_dir=$(mktemp -d)
trap 'rm -rf "${tmp_dir}"' EXIT
rows="${tmp_dir}/rows"
: >"${rows}"

for concurrency in 1 8 32; do
    for ((repeat = 1; repeat <= repeats; ++repeat)); do
        # Alternate the first API at every load so host drift is not correlated
        # with one API always running first.
        if (( (repeat + concurrency) % 2 == 0 )); then
            apis=(core simple)
        else
            apis=(simple core)
        fi
        for api in "${apis[@]}"; do
            binary="${build_dir}/easy_uds_${api}_api_benchmark"
            if [[ ! -x "${binary}" ]]; then
                echo "missing benchmark binary: ${binary}" >&2
                exit 2
            fi
            time_file="${tmp_dir}/${concurrency}-${repeat}-${api}.time"
            if [[ -x /usr/bin/time ]]; then
                output=$(/usr/bin/time -f '%M' -o "${time_file}" \
                    "${binary}" "${iterations}" "${concurrency}")
                rss=$(<"${time_file}")
            else
                output=$("${binary}" "${iterations}" "${concurrency}")
                rss=0
            fi
            throughput=$(awk '/^throughput:/ { print $2 }' <<<"${output}")
            p50=$(sed -n 's/.*p50=\([0-9.e+-]*\) us.*/\1/p' <<<"${output}")
            p99=$(sed -n 's/.*p99=\([0-9.e+-]*\) us.*/\1/p' <<<"${output}")
            cpu=$(awk '/^cpu:/ { print $2 }' <<<"${output}")
            if [[ -z "${throughput}" || -z "${p50}" || -z "${p99}" || -z "${cpu}" ]]; then
                echo "could not parse benchmark output for ${api} c${concurrency}" >&2
                echo "${output}" >&2
                exit 1
            fi
            printf '%s %s %s %s %s %s %s\n' \
                "${concurrency}" "${api}" "${throughput}" "${p50}" \
                "${p99}" "${cpu}" "${rss}" >>"${rows}"
        done
    done
done

median() {
    awk '{ values[NR] = $1 }
         END {
             if (NR == 0) exit 1
             if (NR % 2) {
                 print values[(NR + 1) / 2]
             } else {
                 printf "%.6g\n", (values[NR / 2] + values[NR / 2 + 1]) / 2
             }
         }'
}

median_field() {
    local concurrency=$1
    local api=$2
    local field=$3
    awk -v c="${concurrency}" -v a="${api}" -v f="${field}" \
        '$1 == c && $2 == a { print $f }' "${rows}" | sort -n | median
}

echo "fixed-policy alternating median: iterations=${iterations}, repeats=${repeats}"
if (( repeats == 10 )); then
    echo "decision policy: initial 10 repeats; if any gate is exceeded, rerun with repeats=20 and use all 20"
elif (( repeats == 20 )); then
    echo "decision policy: expanded 20-repeat rerun; all 20 repeats are included"
else
    echo "diagnostic run only: release decisions require a 10-repeat initial run or a 20-repeat expanded rerun"
fi
echo "all reported repeats are included; no passing batch is selected post-hoc"
printf '%-5s %-7s %14s %12s %12s %14s %12s\n' \
    load api throughput_req_s p50_us p99_us cpu_s_1M rss_KiB
for concurrency in 1 8 32; do
    for api in core simple; do
        printf 'c%-4s %-7s %14s %12s %12s %14s %12s\n' \
            "${concurrency}" "${api}" \
            "$(median_field "${concurrency}" "${api}" 3)" \
            "$(median_field "${concurrency}" "${api}" 4)" \
            "$(median_field "${concurrency}" "${api}" 5)" \
            "$(median_field "${concurrency}" "${api}" 6)" \
            "$(median_field "${concurrency}" "${api}" 7)"
    done
done

echo "allocation probe (one run per API):"
for api in core simple; do
    binary="${build_dir}/easy_uds_${api}_api_allocation_benchmark"
    if [[ ! -x "${binary}" ]]; then
        echo "missing allocation benchmark binary: ${binary}" >&2
        exit 2
    fi
    "${binary}" "${iterations}"
done
