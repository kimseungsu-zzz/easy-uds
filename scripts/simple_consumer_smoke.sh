#!/usr/bin/env bash

set -euo pipefail

build_dir=${1:?usage: simple_consumer_smoke.sh <build-dir> [socket-path]}
socket_path=${2:-/tmp/easy-uds-simple-consumer.sock}
log_path="${build_dir}/simple-consumer-server.log"

"${build_dir}/easy_uds_simple_server" "${socket_path}" >"${log_path}" 2>&1 &
server_pid=$!
trap 'kill "${server_pid}" 2>/dev/null || true; wait "${server_pid}" 2>/dev/null || true' EXIT

for _ in $(seq 1 50); do
    if output=$("${build_dir}/easy_uds_simple_client" "${socket_path}" 2>&1); then
        test "$output" = "simple package consumer passed"
        echo "$output"
        exit 0
    fi
    sleep 0.1
done

cat "$log_path"
exit 1
