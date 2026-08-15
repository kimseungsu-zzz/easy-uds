#!/usr/bin/env bash

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cxx=${CXX:-c++}
tmp_dir=$(mktemp -d)
trap 'rm -rf "${tmp_dir}"' EXIT

for source in invalid_assignment invalid_input invalid_return; do
    echo "checking expected Simple API compile failure: ${source} (${cxx})"
    if "${cxx}" -std=c++17 -Wall -Wextra -Wpedantic -I"${root_dir}/include" \
        -I"${root_dir}/experiments/simple_api" -c \
        "${root_dir}/experiments/simple_api/${source}.cpp" \
        -o "${tmp_dir}/${source}.o" 2>"${tmp_dir}/${source}.err"; then
        echo "Simple API invalid source unexpectedly compiled: ${source}" >&2
        exit 1
    fi
    if [[ ! -s "${tmp_dir}/${source}.err" ]]; then
        echo "Simple API compiler produced no diagnostic: ${source}" >&2
        exit 1
    fi
done

echo "Simple API compile-error smoke passed"
