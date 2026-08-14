#!/usr/bin/env bash

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
compiler=${CXX:-c++}
work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT

for source in "$root_dir"/tests/compile_errors/*.cpp; do
    name=$(basename "$source" .cpp)
    log="$work_dir/$name.log"
    echo "checking expected compile failure: $name ($compiler)"
    if "$compiler" -std=c++17 -Wall -Wextra -Wpedantic \
        -I"$root_dir/include" -c "$source" -o "$work_dir/$name.o" \
        >"$log" 2>&1; then
        cat "$log"
        echo "unexpected successful compilation: $source" >&2
        exit 1
    fi
    test -s "$log"
done

echo "compile-error smoke passed"
