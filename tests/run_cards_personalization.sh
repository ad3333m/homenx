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
sanitizer_flags=(-fsanitize=address,undefined -fno-pie -no-pie -g)
c++ -std=gnu++20 -Iprojects/cards/src -Iprojects/common/include -I"$json_include" \
    "${sanitizer_flags[@]}" tests/cards_personalization_test.cpp -o "$test_dir/test"
ASAN_OPTIONS=detect_leaks=0 timeout --kill-after=5s 20s "$test_dir/test"
echo 'Cards personalization tests passed.'
c++ -std=gnu++20 -Iprojects/common/include -I"$json_include" \
    "${sanitizer_flags[@]}" tests/lock_screen_wake_test.cpp -o "$test_dir/wake-test"
ASAN_OPTIONS=detect_leaks=0 timeout --kill-after=5s 20s "$test_dir/wake-test"
echo 'Wake lock-screen and controller notification tests passed.'
nx_include="${DEVKITPRO:-/opt/devkitpro}/libnx/include"
if [[ ! -f "$nx_include/switch/services/ns.h" ]]; then
    echo 'The installed libnx headers are required for the storage protocol test.' >&2
    exit 1
fi
c++ -std=gnu++20 -Itests/support -Iprojects/common/include -I"$nx_include" \
    "${sanitizer_flags[@]}" tests/software_move_test.cpp -o "$test_dir/storage-test"
ASAN_OPTIONS=detect_leaks=0 timeout --kill-after=5s 20s "$test_dir/storage-test"
echo 'Software transfer protocol and cleanup tests passed.'
c++ -std=gnu++20 -Itests/support -Iprojects/cards/src -Iprojects/common/include -I"$nx_include" \
    "${sanitizer_flags[@]}" tests/game_card_test.cpp projects/cards/src/GameCard.cpp \
    -lz -o "$test_dir/game-card-test"
ASAN_OPTIONS=detect_leaks=0 timeout --kill-after=5s 20s "$test_dir/game-card-test"
