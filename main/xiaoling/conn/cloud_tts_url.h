#pragma once

#include <cstring>

// 与 ARCS Mini 的 voice_player_url_is_mp3 一致：只检查 URL 主路径，忽略查询参数。
static bool voice_player_url_is_mp3(const char* url) {
    if (url == nullptr) return false;
    const char* query = std::strchr(url, '?');
    const size_t length = query != nullptr ? static_cast<size_t>(query - url)
                                           : std::strlen(url);
    return length >= 4 && url[length - 4] == '.' &&
           url[length - 3] == 'm' && url[length - 2] == 'p' &&
           url[length - 1] == '3';
}
