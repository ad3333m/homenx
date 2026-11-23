#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
test_dir="$(mktemp -d /tmp/homenx-switube-tests.XXXXXX)"
trap 'rm -rf -- "$test_dir"' EXIT
c++ -std=c++20 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-pie -no-pie -g \
    -Iprojects/cards/src -Iprojects/common/include \
    tests/switube_test.cpp projects/cards/src/Switube.cpp -o "$test_dir/test"
ASAN_OPTIONS=detect_leaks=0 timeout --kill-after=5s 20s "$test_dir/test"
echo 'Switube IPC status and control mapping tests passed.'
c++ -std=c++20 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-pie -no-pie -g \
    -DSWITCHU_MENU -Itests/support/switube -Iprojects/cards/src -Iprojects/common/include -idirafter lib/nxui/include \
    tests/switube_worker_test.cpp projects/cards/src/Switube.cpp -o "$test_dir/worker"
(cd "$test_dir" && ASAN_OPTIONS=detect_leaks=0 timeout --kill-after=5s 20s ./worker)
echo 'Switube startup, controls, browsing and sleep lifecycle tests passed.'
c++ -std=c++20 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-pie -no-pie -g \
    -Iprojects/media/online tests/media_pcm_test.cpp projects/media/online/pcm_output.cpp -o "$test_dir/pcm"
ASAN_OPTIONS=detect_leaks=0 timeout --kill-after=5s 20s "$test_dir/pcm"
echo 'YouTube PCM mixing and playback clock tests passed.'
