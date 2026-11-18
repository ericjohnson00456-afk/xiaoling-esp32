#include "miniapp_hls_playlist.h"
#include <cassert>
#include <iostream>
int main() {
    using namespace MiniappHls;
    assert(Resolve("https://a.test/radio/live.m3u8?token=x", "../audio/01.ts") ==
           "https://a.test/audio/01.ts");
    assert(Resolve("https://a.test/radio/live.m3u8", "/audio/01.ts") ==
           "https://a.test/audio/01.ts");
    assert(Resolve("https://a.test/radio/live.m3u8", "//cdn.test/01.ts") ==
           "https://cdn.test/01.ts");
    Playlist p;
    assert(Parse(
        "#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:42\n#EXTINF:5,\na.ts\n#EXTINF:5,\nb.ts\n#EXT-X-ENDLIST\n",
        p));
    assert(p.sequence == 42 && p.end && p.segments.size() == 2);
    assert(Parse("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=64000\nlow/list.m3u8\n", p));
    assert(p.variant == "low/list.m3u8");
    assert(!Parse("#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=key\na.ts\n", p));
    assert(!Parse("#EXTM3U\n#EXT-X-MAP:URI=init.mp4\na.m4s\n", p));
    assert(!Parse("#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:-1\na.ts\n", p));
    std::cout << "miniapp HLS playlist and relative URL handling passed\n";
}
