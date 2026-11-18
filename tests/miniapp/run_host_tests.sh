#!/bin/sh
set -eu
: "${IDF_PATH:?请先加载 ESP-IDF 环境}"
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT HUP INT TERM
cjson_dir="$IDF_PATH/components/json/cJSON"
cc -std=c11 -Wall -Wextra -Werror -I"$repo_dir/main/miniapp" -I"$cjson_dir" \
    "$repo_dir/tests/miniapp/test_actions.c" "$repo_dir/main/miniapp/miniapp_actions.c" \
    "$cjson_dir/cJSON.c" -lm -o "$work_dir/actions"
cc -std=c11 -Wall -Wextra -Werror -I"$repo_dir/main/miniapp" \
    "$repo_dir/tests/miniapp/test_synth.c" "$repo_dir/main/miniapp/miniapp_synth.c" \
    -lm -o "$work_dir/synth"
c++ -std=c++17 -Wall -Wextra -Werror -I"$repo_dir/main/miniapp" \
    "$repo_dir/tests/miniapp/test_hls_playlist.cc" -o "$work_dir/hls"
"$work_dir/actions"
"$work_dir/synth"
"$work_dir/hls"
