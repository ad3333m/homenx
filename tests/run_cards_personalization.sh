#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."

json_include=""
for candidate in /usr/include "${DEVKITPRO:-/opt/devkitpro}/portlibs/switch/include" \
    "$HOME"/.xmake/packages/n/nlohmann_json/*/*/include; do
    if [[ -f "$candidate/nlohmann/json.hpp" ]]; then json_include="$candidate"; break; fi
done
if [[ -z "$json_include" ]]; then
    echo 'nlohmann/json.hpp is required (install the project dependencies with xmake).' >&2
    exit 1
fi
test_dir="$(mktemp -d /tmp/homenx-cards-tests.XXXXXX)"
trap 'rm -rf -- "$test_dir"' EXIT
c++ -std=gnu++20 -Iprojects/cards/src -Iprojects/common/include -I"$json_include" \
    -fsanitize=address,undefined -g tests/cards_personalization_test.cpp -o "$test_dir/test"
ASAN_OPTIONS=detect_leaks=0 "$test_dir/test"
echo 'Cards personalization tests passed.'
nx_include="${DEVKITPRO:-/opt/devkitpro}/libnx/include"
if [[ ! -f "$nx_include/switch/services/ns.h" ]]; then
    echo 'The installed libnx headers are required for the storage protocol test.' >&2
    exit 1
fi
c++ -std=gnu++20 -Itests/support -Iprojects/common/include -I"$nx_include" \
    -fsanitize=address,undefined -g tests/software_move_test.cpp -o "$test_dir/storage-test"
ASAN_OPTIONS=detect_leaks=0 "$test_dir/storage-test"
echo 'Software transfer protocol and cleanup tests passed.'
