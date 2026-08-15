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

check_cmake_platform_source_set() {
    local list_name=$1
    awk -v name="${list_name}" '
        $0 ~ "^[[:space:]]*set\\(" name "[[:space:]]*$" { inside = 1; next }
        inside && /^[[:space:]]*\)[[:space:]]*$/ { exit }
        inside { print $1 }
    ' "${root_dir}/CMakeLists.txt"
}

check_no_match \
    "CMake backend source assembly must remain explicit" \
    '^[[:space:]]*file[[:space:]]*\([[:space:]]*GLOB' \
    "${root_dir}/CMakeLists.txt"

linux_implementation_files=$(find "${root_dir}/src/system/platform/linux" \
    -type f -name '*.cpp' -printf 'src/system/platform/linux/%f\n' | sort)
cmake_platform_sources=$(check_cmake_platform_source_set EASY_UDS_PLATFORM_SOURCES | sort)
cmake_common_linux_sources=$(check_cmake_platform_source_set EASY_UDS_COMMON_SOURCES |
    grep '^src/system/platform/linux/' || true)

if [[ "${linux_implementation_files}" != "${cmake_platform_sources}" ]]; then
    echo "architecture guard: Linux implementation/source-set mismatch" >&2
    diff -u \
        <(printf '%s\n' "${linux_implementation_files}") \
        <(printf '%s\n' "${cmake_platform_sources}") >&2 || true
    exit 1
fi
if [[ -n "${cmake_common_linux_sources}" ]]; then
    echo "architecture guard: Linux implementation leaked into common CMake sources" >&2
    printf '%s\n' "${cmake_common_linux_sources}" >&2
    exit 1
fi

check_no_match \
    "system/platform/linux must not include user layers" \
    '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]([^>"]*/)?(user/|easy_uds/simple\.hpp)' \
    "${root_dir}/src/system/platform/linux"

check_no_match \
    "system/protocol must not include platform/linux" \
    '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"][^>"]*(platform/linux)[^>"]*[>"]' \
    "${root_dir}/src/system/protocol"

check_no_match \
    "Linux capabilities must not include user-facing public headers" \
    '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]easy_uds/(client|server|session|simple|error|request|response|options|stats|fd|stream|request_context)\.hpp[>"]' \
    "${root_dir}/src/system/platform/linux"

check_no_match \
    "system implementation must not own public POSIX request values" \
    'easy_uds::(OwnedFd|PeerCredentials)' \
    "${root_dir}/src/system"

check_no_match \
    "transport policy must not include Linux backend headers" \
    '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"][^>"]*platform/linux[^>"]*[>"]' \
    "${root_dir}/src/system/transport"

check_no_match \
    "transport policy must not own socket lifecycle or byte syscalls" \
    '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"](sys/socket\.h|unistd\.h|fcntl\.h)[>"]|(^|[^[:alnum:]_])::(socket|connect|accept|bind|listen|send|recv|sendmsg|getsockopt|fcntl|close|shutdown)[[:space:]]*\(' \
    "${root_dir}/src/system/transport"

check_no_match \
    "transport policy must use socket wait capability" \
    'poll\.h|(^|[^[:alnum:]_])::poll[[:space:]]*\(' \
    "${root_dir}/src/system/transport"

for policy_dir in reactor runtime; do
    check_no_match \
        "system/${policy_dir} policy must use socket capabilities" \
        '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]sys/socket\.h[>"]|(^|[^[:alnum:]_])::(socket|connect|accept|bind|listen|send|recv|sendmsg|getsockopt|shutdown)[[:space:]]*\(' \
        "${root_dir}/src/system/${policy_dir}"
done

check_no_match \
    "server runtime must use server pathname capability" \
    'sys/file\.h|sys/stat\.h|unistd\.h|(^|[^[:alnum:]_])::(open|fstat|flock|fchmod|lstat|geteuid|unlink|chmod)[[:space:]]*\(' \
    "${root_dir}/src/system/runtime/server.cpp"

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
    check_no_match \
        "system policy/state code must not own Linux descriptor ancillary operations" \
        'SCM_RIGHTS|MSG_CTRUNC|MSG_CMSG_CLOEXEC|CMSG_[A-Z_]+|cmsghdr' \
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
