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

# Compile the production runtime against the embedded Lua sources. These stubs
# stop at the RTOS/network boundary; they do not validate scheduling or hardware.
mkdir -p "$work_dir/include/freertos"
for header in sdkconfig.h esp_heap_caps.h esp_log.h esp_http_client.h esp_crt_bundle.h \
    freertos/FreeRTOS.h freertos/queue.h freertos/semphr.h freertos/task.h freertos/idf_additions.h; do
    printf '#include "runtime_host.h"\n' > "$work_dir/include/$header"
done
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter \
    -I"$work_dir/include" -I"$repo_dir/tests/miniapp" -I"$repo_dir/main/miniapp" \
    -I"$repo_dir/components/lua/src" -I"$cjson_dir" \
    "$repo_dir/tests/miniapp/test_runtime.c" "$repo_dir/components/lua/src/"*.c \
    "$repo_dir/main/miniapp/miniapp_lua_screen.c" \
    "$repo_dir/main/miniapp/miniapp_actions.c" "$cjson_dir/cJSON.c" \
    -lm -o "$work_dir/runtime"
"$work_dir/runtime"
