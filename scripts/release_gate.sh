#!/usr/bin/env bash

set -euo pipefail

# One-command Linux release gate. This validates the installed package as
# well as in-tree targets; it does not create tags or releases.

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
base_build_dir=${1:-"${root_dir}/build-release-gate"}

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "release_gate: Linux is required (epoll and AF_UNIX)" >&2
    exit 2
fi

bash "${root_dir}/scripts/check_architecture.sh"

run_consumer() {
    local variant_build=$1
    local prefix_dir=$2
    local suffix=$3
    local consumer_build="${variant_build}/package-consumer"
    local beginner_build="${variant_build}/beginner-consumer"
    local simple_build="${variant_build}/simple-consumer"
    local socket_path="/tmp/easy-uds-release-gate-${suffix}.sock"

    cmake --install "${variant_build}" --prefix "${prefix_dir}"

    cmake -S "${root_dir}/tests/package_consumer" \
        -B "${consumer_build}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="${prefix_dir}"
    cmake --build "${consumer_build}" --parallel

    cmake -S "${root_dir}/tests/beginner" \
        -B "${beginner_build}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="${prefix_dir}"
    cmake --build "${beginner_build}" --parallel

    cmake -S "${root_dir}/tests/simple_consumer" \
        -B "${simple_build}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="${prefix_dir}"
    cmake --build "${simple_build}" --parallel

    local ld_path="${prefix_dir}/lib"
    if [[ -n "${LD_LIBRARY_PATH:-}" ]]; then
        ld_path="${ld_path}:${LD_LIBRARY_PATH}"
    fi
    LD_LIBRARY_PATH="${ld_path}" \
        "${consumer_build}/easy_uds_package_consumer"
    LD_LIBRARY_PATH="${ld_path}" \
        bash "${root_dir}/scripts/beginner_consumer_smoke.sh" \
            "${beginner_build}" "${socket_path}"
    LD_LIBRARY_PATH="${ld_path}" \
        bash "${root_dir}/scripts/simple_consumer_smoke.sh" \
            "${simple_build}" "/tmp/easy-uds-release-gate-simple-${suffix}.sock"
}

run_variant() {
    local shared=$1
    local suffix
    suffix=$(tr '[:upper:]' '[:lower:]' <<<"${shared}")
    local variant_build="${base_build_dir}-${suffix}"
    local prefix_dir="${variant_build}/install"

    echo
    echo "=== release gate: BUILD_SHARED_LIBS=${shared} ==="
    cmake -S "${root_dir}" -B "${variant_build}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS="${shared}" \
        -DEASY_UDS_BUILD_EXAMPLES=ON \
        -DEASY_UDS_BUILD_TESTS=ON \
        -DEASY_UDS_BUILD_BENCHMARKS=ON \
        -DEASY_UDS_WARNINGS_AS_ERRORS=ON
    cmake --build "${variant_build}" --parallel

    ctest --test-dir "${variant_build}" --output-on-failure
    ctest --test-dir "${variant_build}" -L rc --output-on-failure
    CXX="${CXX:-${CMAKE_CXX_COMPILER:-c++}}" \
        bash "${root_dir}/scripts/compile_error_smoke.sh"
    run_consumer "${variant_build}" "${prefix_dir}" "${suffix}"
}

run_variant OFF
run_variant ON

echo
echo "release_gate: 0.8 final build, unit/integration, release labels, compile-error UX,"
echo "release_gate: static/shared package consumers, Simple API /echo smoke,"
echo "release_gate: and beginner /echo smoke passed"
