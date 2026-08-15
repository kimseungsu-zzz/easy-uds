#!/usr/bin/env bash

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

check_no_match() {
    local label=$1
    local pattern=$2
    shift 2
    local matches
    matches=$(grep -RInE --exclude='*.md' --exclude='*.txt' "${pattern}" "$@" || true)
    if [[ -n "${matches}" ]]; then
        echo "architecture guard: ${label}" >&2
        echo "${matches}" >&2
        exit 1
    fi
}

check_no_match \
    "system/platform/linux must not include user layers" \
    '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]([^>"]*/)?(user/|easy_uds/simple\.hpp)' \
    "${root_dir}/src/system/platform/linux"

check_no_match \
    "system/protocol must not include platform/linux" \
    '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"][^>"]*(platform/linux)[^>"]*[>"]' \
    "${root_dir}/src/system/protocol"

system_sources=()
while IFS= read -r -d '' path; do
    system_sources+=("${path}")
done < <(find "${root_dir}/src/system" -type f \
    ! -path "${root_dir}/src/system/platform/linux/*" \
    \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) -print0)
if ((${#system_sources[@]} > 0)); then
    check_no_match \
        "system policy/state code must not include Linux readiness headers" \
        '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"](sys/epoll\.h|sys/eventfd\.h)[>"]' \
        "${system_sources[@]}"
    check_no_match \
        "system policy/state code must not own SO_PEERCRED" \
        'SO_PEERCRED|struct[[:space:]]+ucred' \
        "${system_sources[@]}"
fi

user_sources=()
while IFS= read -r -d '' path; do
    user_sources+=("${path}")
done < <(find "${root_dir}/src/user" -type f \( -name '*.cpp' -o -name '*.c' -o -name '*.py' \) -print0)
if ((${#user_sources[@]} > 0)); then
    check_no_match \
        "user implementation must not include platform/linux" \
        '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"][^>"]*(platform/linux|sys/epoll|sys/eventfd|sys/socket\.h|sys/un\.h|unistd\.h)[^>"]*[>"]' \
        "${user_sources[@]}"
fi

echo "architecture_guard: user -> system -> platform direction passed"
