#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <cstdlib>

namespace MiniappHls {
struct Playlist {
    uint64_t sequence = 0;
    bool end = false;
    std::string variant;
    std::vector<std::string> segments;
};
inline std::string Resolve(const std::string &base, const std::string &path) {
    if (path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0)
        return path;
    auto scheme = base.find("://");
    if (scheme == std::string::npos)
        return {};
    if (path.rfind("//", 0) == 0)
        return base.substr(0, scheme + 1) + path;
    auto host_end = base.find('/', scheme + 3);
    const auto host = base.substr(0, host_end);
    if (!path.empty() && path[0] == '/')
        return host + path;
    auto clean = base.substr(0, base.find_first_of("?#"));
    if (!path.empty() && path[0] == '?')
        return clean + path;
    auto slash = clean.find_last_of('/');
    auto result = (host_end == std::string::npos ? host + "/" : clean.substr(0, slash + 1)) + path;
    // Resolve dot segments without touching URL parameters.
    auto query = result.find_first_of("?#");
    auto suffix = query == std::string::npos ? "" : result.substr(query);
    auto fullpath =
        result.substr(host.size(), query == std::string::npos ? query : query - host.size());
    std::vector<std::string> parts;
    size_t from = 1;
    while (from <= fullpath.size()) {
        auto end = fullpath.find('/', from);
        auto part = fullpath.substr(from, end - from);
        if (part == "..") {
            if (!parts.empty())
                parts.pop_back();
        } else if (part != ".")
            parts.push_back(part);
        if (end == std::string::npos)
            break;
        from = end + 1;
    }
    result = host;
    for (const auto &part : parts)
        result += "/" + part;
    return result + suffix;
}
inline bool Parse(const std::string &text, Playlist &out) {
    if (text.rfind("#EXTM3U", 0) != 0 || text.size() > 32768)
        return false;
    out = {};
    bool variant = false;
    for (size_t begin = 0; begin < text.size();) {
        auto end = text.find('\n', begin);
        auto line = text.substr(begin, end - begin);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        begin = end == std::string::npos ? text.size() : end + 1;
        if (line.empty())
            continue;
        if (line.rfind("#EXT-X-MEDIA-SEQUENCE:", 0) == 0) {
            char *tail = nullptr;
            const char *value = line.c_str() + 22;
            if (!*value || *value == '-' || *value == '+')
                return false;
            out.sequence = strtoull(value, &tail, 10);
            if (!tail || *tail || out.sequence > 9007199254740991ULL)
                return false;
        } else if (line == "#EXT-X-ENDLIST")
            out.end = true;
        else if (line.rfind("#EXT-X-STREAM-INF:", 0) == 0)
            variant = true;
        else if (line.rfind("#EXT-X-KEY:", 0) == 0 && line != "#EXT-X-KEY:METHOD=NONE")
            return false;
        else if (line.rfind("#EXT-X-MAP:", 0) == 0 || line.rfind("#EXT-X-BYTERANGE:", 0) == 0)
            return false;
        else if (line[0] != '#') {
            if (line.size() > 2047 || line.find_first_of(" \t\r") != std::string::npos)
                return false;
            if (variant) {
                if (out.variant.empty())
                    out.variant = line;
                variant = false;
            } else {
                if (out.segments.size() >= 128)
                    return false;
                out.segments.push_back(line);
            }
        }
    }
    return !out.variant.empty() || !out.segments.empty();
}
} // namespace MiniappHls
