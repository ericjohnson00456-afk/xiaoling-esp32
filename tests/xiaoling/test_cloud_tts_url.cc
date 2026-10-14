#include "../../main/xiaoling/conn/cloud_tts_url.h"

#include <cassert>

int main() {
    assert(!voice_player_url_is_mp3(nullptr));
    assert(!voice_player_url_is_mp3("https://example.com/tts"));
    assert(!voice_player_url_is_mp3("https://example.com/tts?name=a.mp3"));
    assert(voice_player_url_is_mp3("https://example.com/tts.mp3"));
    assert(voice_player_url_is_mp3("https://example.com/tts.mp3?token=abc"));
    assert(!voice_player_url_is_mp3("https://example.com/tts.mp3.extra"));
}
